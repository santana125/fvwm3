/* fmd - fvwm module daemon
 *
 * This program accepts listeners over a UDS for the purposes of receiving
 * information from FVWM3.
 *
 * Released under the same license as FVWM3 itself.
 */

#include "config.h"

#include "fvwm/fvwm.h"

#include "libs/Module.h"
#include "libs/safemalloc.h"
#include "libs/queue.h"
#include "libs/fvwmsignal.h"
#include "libs/vpacket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>

#include <bson/bson.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/listener.h>

/* XXX - should also be configurable via getenv() */
#define SOCK "/tmp/fvwm_fmd.sock"

const char *myname = "fmd";
static int debug;

struct client {
	struct bufferevent	*comms;
	int			 flags;

	TAILQ_ENTRY(client)	 entry;
};

TAILQ_HEAD(clients, client);
struct clients  clientq;

struct fvwm_comms {
	int		 fd[2];
	struct event	*read_ev;
	ModuleArgs	*m;
};

struct fvwm_comms	 fc;

struct event_flag {
	const char	*event;
	int		 flag;
} etf[] = {
	{"new_window", M_ADD_WINDOW},
	{"configure_window", M_CONFIGURE_WINDOW},
	{"new_page", M_NEW_PAGE},
	{"new_desk", M_NEW_DESK},
	{"raise_window", M_RAISE_WINDOW},
	{"lower_window", M_LOWER_WINDOW},
	{"focus_change", M_FOCUS_CHANGE},
	{"destroy_window", M_DESTROY_WINDOW},
	{"iconify", M_ICONIFY},
	{"deiconify", M_DEICONIFY},
	{"window_name", M_WINDOW_NAME},
	{"icon_name", M_ICON_NAME},
	{"res_class", M_RES_CLASS},
	{"res_name", M_RES_NAME},
	{"icon_location", M_ICON_LOCATION},
	{"map", M_MAP},
	{"icon_file", M_ICON_FILE},
	{"window_shade", M_WINDOWSHADE},
	{"dewindow_shade", M_DEWINDOWSHADE},
	{"restack", M_RESTACK},
};


static void fvwm_read(int, short, void *);
static void broadcast_to_client(FvwmPacket *);
static void setup_signal_handlers(void);
static RETSIGTYPE HandleTerminate(int sig);
static int client_set_interest(struct client *, const char *);
static bson_t *handle_packet(int, unsigned long *, unsigned long);

static RETSIGTYPE
HandleTerminate(int sig)
{
	unlink(SOCK);
	fvwmSetTerminate(sig);
	SIGNAL_RETURN;
}

static void
setup_signal_handlers(void)
{
    struct sigaction  sigact;

    memset(&sigact, 0, sizeof sigact);

    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGTERM);
    sigaddset(&sigact.sa_mask, SIGINT);
    sigaddset(&sigact.sa_mask, SIGQUIT);
    sigaddset(&sigact.sa_mask, SIGPIPE);
    sigaddset(&sigact.sa_mask, SIGCHLD);
    sigact.sa_flags = SA_INTERRUPT; /* to interrupt ReadFvwmPacket() */
    sigact.sa_handler = HandleTerminate;

    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
    sigaction(SIGCHLD, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
}

static bson_t *
handle_packet(int type, unsigned long *body, unsigned long len)
{
	bson_t	*doc = NULL;

	switch(type) {
	case M_ADD_WINDOW:
	case M_CONFIGURE_WINDOW: {
		struct ConfigWinPacket *cwp = (void *)body;
		doc = BCON_NEW(
		    "w", BCON_INT64(cwp->w),
		    "frame", BCON_INT64(cwp->frame),
		    "frame_x", BCON_INT64(cwp->frame_x),
		    "frame_y", BCON_INT64(cwp->frame_y)
		);
		return (doc);
	}
	default:
		break;
	}

	return (NULL);
}

static inline const char *
flag_to_event(int flag)
{
	size_t	 i;

	for (i = 0; i < (sizeof(etf) / sizeof(etf[0])); i++) {
		if (etf[i].flag & flag)
			return (etf[i].event);
	}

	return (NULL);
}

static int
setnonblock(int fd)
{
        int flags;

        if ((flags = fcntl(fd, F_GETFL)) < 0)
                return (flags);

        flags |= O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) < 0)
                return (-1);

        return (0);
}

static inline bool
strsw(const char *pre, const char *str)
{
	return (strncmp(pre, str, strlen(pre)) == 0);
}

#define EFLAGSET	0x1
#define EFLAGUNSET	0x2

static int
client_set_interest(struct client *c, const char *event)
{
	size_t		i;
	int		flag_type = 0;
	bool		changed = false;
#define PRESET "set"
#define PREUNSET "unset"

	if (strsw(PRESET, event)) {
		event += strlen(PRESET) + 1;
		flag_type = EFLAGSET;
	} else if (strsw(PREUNSET, event)) {
		event += strlen(PREUNSET) + 1;
		flag_type = EFLAGUNSET;
	}

	if (strcmp(event, "debug") == 0) {
		debug = (flag_type == EFLAGSET) ? 1 : 0;

		/* Never send to FVWM3. */
		return (true);
	}


	for (i = 0; i < (sizeof(etf) / sizeof(etf[0])); i++) {
		if (strcmp(etf[i].event, event) == 0) {
			changed = true;
			if (flag_type == EFLAGSET)
				c->flags |= etf[i].flag;
			else
				c->flags &= ~etf[i].flag;
		}
	}

	SetSyncMask(fc.fd, c->flags);

	return (changed);
}

static void
broadcast_to_client(FvwmPacket *packet)
{
	bson_t			*doc;
	char			*as_json;
	struct client		*c;
	size_t			 json_len;
	unsigned long		*body = packet->body;
	unsigned long		 type =	packet->type;
	unsigned long		 length = packet->size;

	TAILQ_FOREACH(c, &clientq, entry) {
		if (!(c->flags & type))
			continue;

		if ((doc = handle_packet(type, body, length)) == NULL)
			continue;

		as_json = bson_as_relaxed_extended_json(doc, &json_len);
		if (as_json == NULL) {
			bson_free(as_json);
			continue;
		}

		bufferevent_write(c->comms, as_json, strlen(as_json));
		bson_free(as_json);
		bson_destroy(doc);
	}
}

static void
client_read_cb(struct bufferevent *bev, void *ctx)
{
	struct evbuffer	*input = bufferevent_get_input(bev);
	struct client	*c = (struct client *)ctx;
	size_t		 len = evbuffer_get_length(input);
	char		 *data = fxmalloc(len);

	evbuffer_copyout(input, data, len);

	/* Remove the newline if there is one. */
	if (data[strlen(data) - 1] == '\n')
		data[strlen(data) - 1] = '\0';

	if (*data == '\0')
		goto out;

	if (client_set_interest(c, data))
		goto out;

	SendText(fc.fd, data, 0);

out:
	free(data);
	evbuffer_drain(input, len);
}

static void
client_write_cb(struct bufferevent *bev, void *ctx)
{
	struct client	*c = ctx;

	if (debug)
		fprintf(stderr, "Writing... (client %p)...\n", c);
}

static void client_err_cb(struct bufferevent *bev, short events, void *ctx)
{
	struct client	*c = (struct client *)ctx, *clook;

	if (events & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		TAILQ_FOREACH(clook, &clientq, entry) {
			if (c == clook) {
				TAILQ_REMOVE(&clientq, c, entry);
				bufferevent_free(bev);
			}
		}
	}
}


static void
accept_conn_cb(struct evconnlistener *l, evutil_socket_t fd,
		struct sockaddr *add, int socklen, void *ctx)
{
	/* We got a new connection! Set up a bufferevent for it. */
	struct client		*c;
	struct event_base	*base = evconnlistener_get_base(l);

	c = fxmalloc(sizeof *c);

	c->comms = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);

	bufferevent_setcb(c->comms, client_read_cb, client_write_cb,
	    client_err_cb, c);
	bufferevent_enable(c->comms, EV_READ|EV_WRITE|EV_PERSIST);

	TAILQ_INSERT_TAIL(&clientq, c, entry);
}

static void accept_error_cb(struct evconnlistener *listener, void *ctx)
{
	struct event_base *base = evconnlistener_get_base(listener);
	int err = EVUTIL_SOCKET_ERROR();
	fprintf(stderr, "Got an error %d (%s) on the listener. "
		"Shutting down.\n", err, evutil_socket_error_to_string(err));

	event_base_loopexit(base, NULL);
}

static void
fvwm_read(int efd, short ev, void *data)
{
	FvwmPacket	*packet;

	SendUnlockNotification(fc.fd);

	if ((packet = ReadFvwmPacket(efd)) == NULL) {
		if (debug)
			fprintf(stderr, "Couldn't read from FVWM - exiting.\n");
		exit(0);
	}

	broadcast_to_client(packet);
}

int main(int argc, char **argv)
{
	struct event_base     *base;
	struct evconnlistener *fmd_cfd;
	struct sockaddr_un    sin;

	TAILQ_INIT(&clientq);

	setup_signal_handlers();

	if ((fc.m = ParseModuleArgs(argc, argv, 1)) == NULL) {
		fprintf(stderr, "%s must be started by FVWM3\n", myname);
		return (1);
	}

	/* Create new event base */
	if ((base = event_base_new()) == NULL) {
		fprintf(stderr, "Couldn't start libevent\n");
		return (1);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sun_family = AF_LOCAL;
	strcpy(sin.sun_path, SOCK);

	/* Create a new listener */
	fmd_cfd = evconnlistener_new_bind(base, accept_conn_cb, NULL,
		  LEV_OPT_CLOSE_ON_FREE, -1,
		  (struct sockaddr *)&sin, sizeof(sin));
	if (fmd_cfd == NULL) {
		perror("Couldn't create listener");
		return 1;
	}
	evconnlistener_set_error_cb(fmd_cfd, accept_error_cb);

	/* Setup comms to fvwm3. */
	fc.fd[0] = fc.m->to_fvwm;
	fc.fd[1] = fc.m->from_fvwm;

	if (setnonblock(fc.fd[0]) < 0)
		fprintf(stderr, "fvwm to_fvwm socket non-blocking failed");
	if (setnonblock(fc.fd[1]) < 0)
		fprintf(stderr, "fvwm to_fvwm socket non-blocking failed");

	fc.read_ev = event_new(base, fc.fd[1], EV_READ|EV_PERSIST, fvwm_read, NULL);
	event_add(fc.read_ev, NULL);

	SendFinishedStartupNotification(fc.fd);

	event_base_dispatch(base);

	return (0);
}
