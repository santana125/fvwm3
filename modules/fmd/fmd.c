/* fmd - fvwm module daemon
 *
 * This program accepts listeners over a UDS for the purposes of receiving
 * information from FVWM3.
 *
 * Released under the same license as FVWM3 itself.
 */

#include "config.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <stdbool.h>

#include <bson.h>
#include <event.h>

#include "libs/Module.h"
#include "libs/safemalloc.h"
#include "libs/queue.h"
#include "libs/fvwmsignal.h"

#define SOCKET_PATH "/tmp/fvwm_fmd.sock"
#define BUFLEN 1024

const char *myname = "fmd";
struct event fvwm_read, fvwm_write;
static ModuleArgs *module;
static int fvwm_fd[2];

struct buffer;
struct client;

static int setnonblock(int);

static void on_client_read(int, short, void *);
static void on_client_write(int, short, void *);
static void on_client_accept(int, short, void *);

static void on_fvwm_read(int, short, void *);
static void on_fvwm_write(int, short, void *);

static void broadcast_to_client(unsigned long);
static int write_to_fd(int, struct event *, struct buffer *);

static void setup_signal_handlers(void);
static RETSIGTYPE HandleTerminate(int sig);

static int client_set_interest(struct client *, const char *);

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

static RETSIGTYPE
HandleTerminate(int sig)
{
	fvwmSetTerminate(sig);
	exit(0);
	SIGNAL_RETURN;
}

static void
setup_signal_handlers(void)
{
    struct sigaction  sigact;

    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGTERM);
    sigaddset(&sigact.sa_mask, SIGINT);
    sigaddset(&sigact.sa_mask, SIGPIPE);
#ifdef SA_INTERRUPT
    sigact.sa_flags = SA_INTERRUPT; /* to interrupt ReadFvwmPacket() */
#else
    sigact.sa_flags = 0;
#endif
    sigact.sa_handler = HandleTerminate;

    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGINT,  &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
    signal(SIGTERM, HandleTerminate);
    signal(SIGINT,  HandleTerminate);
    signal(SIGPIPE, HandleTerminate);     /* Dead pipe == fvwm died */
#ifdef HAVE_SIGINTERRUPT
    siginterrupt(SIGTERM, True);
    siginterrupt(SIGINT,  True);
    siginterrupt(SIGPIPE, True);
#endif
}

struct client {
	struct event	read, write;
	u_int		flags;

	TAILQ_ENTRY(client)  entry;
	TAILQ_HEAD(, buffer) c_writeq;
	TAILQ_HEAD(, buffer) f_writeq;
};
TAILQ_HEAD(clients, client);
struct clients	clientq;

struct buffer {
	int 	 offset, len;
	char 	*buf;

	TAILQ_ENTRY(buffer) entries;
};

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

static int
write_to_fd(int fd, struct event *e, struct buffer *bufferq)
{
	int		 len;

	/* Write the buffer.  A portion of the buffer may have been
	 * written in a previous write, so only write the remaining
	 * bytes.
	 */

	len = bufferq->len - bufferq->offset;
	len = write(fd, bufferq->buf + bufferq->offset,
			bufferq->len - bufferq->offset);
	if (len == -1) {
		if (errno == EINTR || errno == EAGAIN) {
			/* Write failed. */
			event_add(e, NULL);
			return (-1);
		} else
			err(1, "write failed");
	} else if ((bufferq->offset + len) < bufferq->len) {
		/* Incomplete write - reschedule. */
		bufferq->offset += len;
		event_add(e, NULL);
		return (-1);
	}
	return (0);
}

static void
broadcast_to_client(unsigned long type)
{
	struct client	*c;
	struct buffer 	*bufferq;
	char 		*buf;

	TAILQ_FOREACH(c, &clientq, entry) {
		if (!(c->flags & type))
			continue;
		asprintf(&buf, "Reacting to: %s", flag_to_event(type));

		bufferq = fxmalloc(sizeof(*bufferq));
		bufferq->len = strlen(buf);
		bufferq->buf = fxstrdup(buf);
		bufferq->offset = 0;
		TAILQ_INSERT_TAIL(&c->c_writeq, bufferq, entries);

		free(buf);

		event_add(&c->write, NULL);
	}
}

static void
on_fvwm_read(int fd, short ev, void *arg)
{
	FvwmPacket	*packet;

	if ((packet = ReadFvwmPacket(fd)) == NULL) {
		fprintf(stderr, "Couldn't read from FVWM - exiting.\n");
		goto done;
	}

	broadcast_to_client(packet->type);
done:
	SendUnlockNotification(fvwm_fd);
}

static void
on_fvwm_write(int fd, short ev, void *arg)
{
	struct client 	*c = (struct client *)arg;
	struct buffer	*b;

	if (c == NULL) {
		fprintf(stderr, "%s: not continuing.  Client is NULL\n", __func__);
		return;
	}

	TAILQ_FOREACH(b, &c->f_writeq, entries) {
		fprintf(stderr, "Sent: <<%s>>\n", b->buf);
		SendText(fvwm_fd, b->buf, 0);
	}

	TAILQ_FOREACH(b, &c->f_writeq, entries) {
		/* Write complete.  Remove from the buffer. */
		TAILQ_REMOVE(&c->f_writeq, b, entries);
		free(b->buf);
		free(b);
	}
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
	int		flag_type;
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

	for (i = 0; i < (sizeof(etf) / sizeof(etf[0])); i++) {
		if (strcmp(etf[i].event, event) == 0) {
			changed = true;
			if (flag_type == EFLAGSET)
				c->flags |= etf[i].flag;
			else
				c->flags &= ~etf[i].flag;
		}
	}

	return (changed);
}

static void
on_client_read(int fd, short ev, void *arg)
{
	struct client 	*client = (struct client *)arg;
	struct buffer	*bufferq, *b;
	char		*buf;
	int		 len;

	buf = fxmalloc(BUFLEN);

	if ((len = read(fd, buf, BUFLEN)) <= 0) {
		free(buf);
		fprintf(stderr, "client disconnected.\n");
		close(fd);
		event_del(&client->read);
		TAILQ_REMOVE(&clientq, client, entry);

		TAILQ_FOREACH(b, &client->f_writeq, entries) {
			TAILQ_REMOVE(&client->f_writeq, b, entries);
			free(b->buf);
			free(b);
		}
		free(client);
		return;
	}

	/* Remove the newline if there is one. */
	if (buf[strlen(buf) - 1] == '\n')
		buf[strlen(buf) - 1] = '\0';

	if (*buf == '\0') {
		free(buf);
		return;
	}

	if (client_set_interest(client, buf))
		return;

	/* Anything else is sent through to FVWM3. */
	bufferq = fxcalloc(1, sizeof(*bufferq));
	bufferq->len = strlen(buf);
	bufferq->buf = fxstrdup(buf);
	bufferq->offset = 0;
	TAILQ_INSERT_TAIL(&client->f_writeq, bufferq, entries);

	free(buf);

	event_del(&fvwm_write);
	event_set(&fvwm_write, fvwm_fd[0], EV_WRITE, on_fvwm_write, client);
	event_add(&fvwm_write, NULL);
}

static void
on_client_write(int fd, short ev, void *arg)
{
	struct client	*c = (struct client *)arg;
	struct buffer	*b;

	if (c == NULL)
		return;

	b = TAILQ_FIRST(&c->c_writeq);

	if (b != NULL && (write_to_fd(fd, &c->write, b)) == 0) {
		/* Write complete.  Remove from the buffer. */
		TAILQ_REMOVE(&c->c_writeq, b, entries);
		free(b->buf);
		free(b);
	}
}

static void
on_client_accept(int fd, short ev, void *arg)
{
	struct sockaddr_in 	 client_addr;
	struct client 		*client;
	socklen_t 		 client_len = sizeof(client_addr);
	int 			 client_fd;

	/* Accept the new connection. */
	client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd == -1) {
		fprintf(stderr, "accept failed: %s", strerror(errno));
		return;
	}

	if (setnonblock(client_fd) < 0)
		fprintf(stderr, "couldn't make socket non-blocking\n");

	client = fxmalloc(sizeof(*client));

	event_set(&client->read, client_fd, EV_READ|EV_PERSIST, on_client_read,
	    client);

	/* Make the event active, by adding it. */
	event_add(&client->read, NULL);

	/* Setting the event here makes libevent aware of it, but we don't
	 * want to make it active yet via event_add() until we're ready.
	 */
	event_set(&client->write, client_fd, EV_WRITE, on_client_write, client);
	event_set(&fvwm_write, fvwm_fd[0], EV_WRITE, on_fvwm_write, client);

	TAILQ_INIT(&client->c_writeq);
	TAILQ_INIT(&client->f_writeq);
	TAILQ_INSERT_TAIL(&clientq, client, entry);

	event_add(&fvwm_write, NULL);

	fprintf(stderr, "Accepted connection...\n");
}

int
main(int argc, char **argv)
{
	struct sockaddr_un addr;
	struct event ev_accept;
	int fd;

	if ((module = ParseModuleArgs(argc, argv, 1)) == NULL) {
		fprintf(stderr, "%s must be started by FVWM3\n", myname);
		exit(1);
	}

	setup_signal_handlers();

	fvwm_fd[0] = module->to_fvwm;
	fvwm_fd[1] = module->from_fvwm;

	event_init();

	TAILQ_INIT(&clientq);

	/* Set up the read/write listeners as early as possible, as there's
	 * every chance FVWM will send us information as soon as we start up.
	 */
	if (setnonblock(fvwm_fd[0]) < 0)
		err(1, "fvwm to_fvwm socket non-blocking failed");
	if (setnonblock(fvwm_fd[1]) < 0)
		err(1, "fvwm from_fvwm socket non-blocking failed");

	event_set(&fvwm_read, fvwm_fd[1], EV_READ|EV_PERSIST, on_fvwm_read, NULL);
	event_add(&fvwm_read, NULL);

	SendFinishedStartupNotification(fvwm_fd);
	SetSyncMask(fvwm_fd, M_ADD_WINDOW);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		err(1, "listen failed");

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (unlink(addr.sun_path) != 0 && errno != ENOENT)
		err(1, "unlink failed");

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		err(1, "bind failed");
	if (listen(fd, 1024) < 0)
		err(1, "listen failed");
	if (setnonblock(fd) < 0)
		err(1, "server socket non-blocking failed");

	event_set(&ev_accept, fd, EV_READ|EV_PERSIST, on_client_accept, NULL);
	event_add(&ev_accept, NULL);

	fprintf(stderr, "Started.  Waiting for connections...\n");

	event_dispatch();

	return (0);
}
