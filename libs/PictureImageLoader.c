/* -*-c-*- */
/* Copyright (C) 1993, Robert Nation
 * Copyright (C) 1999 Carsten Haitzler and various contributors (imlib2)
 * Copyright (C) 2002  Olivier Chapuis */
/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *
 * The png loader and PImageRGBtoPixel are from imlib2. The code is from raster
 * (Carsten Haitzler) <raster@rasterman.com> <raster@valinux.com>
 *
 */

/* ---------------------------- included header files ---------------------- */

#include "config.h"

#include <stdio.h>
#include <signal.h>
#include <ctype.h>

#include <X11/Xlib.h>
#include <X11/Xmd.h>

#include <fvwmlib.h>
#include "System.h"
#include "Strings.h"
#include "Picture.h"
#include "PictureUtils.h"
#include "Graphics.h"
#include "Fxpm.h"
#include "Fpng.h"
#include "Fsvg.h"
#include "FRenderInit.h"
#include "FImage.h"

/* ---------------------------- local definitions -------------------------- */
#define FIMAGE_CMD_ARGS \
	Display *dpy, char *path, CARD32 **argb_data, int *width, int *height

#define FIMAGE_PASS_ARGS \
	dpy, path, argb_data, width, height

typedef struct PImageLoader
{
  char *extension;
#ifdef __STDC__
  int (*func)(FIMAGE_CMD_ARGS);
#else
  int (*func)();
#endif
} PImageLoader;

/* ---------------------------- local macros ------------------------------- */

/* ---------------------------- imports ------------------------------------ */

/* ---------------------------- included code files ------------------------ */

/* ---------------------------- local types -------------------------------- */

/* ---------------------------- forward declarations ----------------------- */

static Bool PImageLoadSvg(FIMAGE_CMD_ARGS);
static Bool PImageLoadPng(FIMAGE_CMD_ARGS);
static Bool PImageLoadXpm(FIMAGE_CMD_ARGS);

/* ---------------------------- local variables ---------------------------- */

PImageLoader Loaders[] =
{
	{ "xpm", PImageLoadXpm },
	{ "svg", PImageLoadSvg },
	{ "png", PImageLoadPng },
	{NULL,0}
};

/* ---------------------------- exported variables (globals) --------------- */

/* ---------------------------- local functions ---------------------------- */

static
Bool PImageLoadArgbDataFromFile(FIMAGE_CMD_ARGS)
{
	int done = 0, i = 0, tried = -1;
	char *ext = NULL;

	if (path == NULL)
		return False;

	if (strlen(path) > 3)
	{
		ext = path + strlen(path) - 3;
	}
	/* first try to load by extension */
	while(!done && ext != NULL && Loaders[i].extension != NULL)
	{
		if (StrEquals(Loaders[i].extension, ext))
		{
			if (Loaders[i].func(FIMAGE_PASS_ARGS))
			{
				return True;
			}
			tried = i;
			done = 1;
		}
		i++;
	}

	i = 0;
	while(Loaders[i].extension != NULL)
	{
		if (i != tried && Loaders[i].func(FIMAGE_PASS_ARGS))
		{
			return True;
		}
		i++;
	}

	return False;
}

/*
 *
 * svg loader
 *
 */
static
Bool PImageLoadSvg(FIMAGE_CMD_ARGS)
{
	char *allocated_path;
	char *render_opts;
	FRsvgHandle *rsvg;
	FRsvgDimensionData dim;
	CARD32 *data;
	Fcairo_surface_t *surface;
	Fcairo_t *cr;
	int i;
	int j;
	int b1;
	int b2;
	int w = 0;
	int h = 0;
	int dw = 0;
	int dh = 0;
	int w_sgn = 1;
	int h_sgn = 1;
	double angle = 0;
	double scale = 1;
	double buf;
	unsigned char a_value;
	unsigned char r_value;
	unsigned char g_value;
	unsigned char b_value;

	if (!USE_SVG)
	{
		return False;
	}

	/* Separate rendering options from path */
	render_opts = path = allocated_path = safestrdup(path);
	if (*path == ':' && (path = strchr(path + 1, ':')))
	{
		*path = 0;
		path ++;
		render_opts ++;
	}
	else
	{
		render_opts = "";
	}

	if (!(rsvg = Frsvg_handle_new_from_file(path, NULL)))
	{
		free(allocated_path);

		return False;
	}

	/* Parsing of rendering options */
	while (*render_opts)
	{
		i = 0;
		switch (*render_opts)
		{
		case '*':
			if (sscanf(render_opts, "*%lf%n", &buf, &i) >= 1)
			{
				scale *= buf;
			}
			break;
		case '/':
			if (sscanf(render_opts, "/%lf%n", &buf, &i) >= 1 &&
			    buf)
			{
				scale /= buf;
			}
			break;
		case '@':
			if (sscanf(render_opts, "@%lf%n", &buf, &i) >= 1)
			{
				angle += buf * M_PI / 180;
			}
			break;
		default:
			j = 0;
			if (
				sscanf(
					render_opts, "%dx%n%d%n", &b1, &j, &b2,
					&i) >= 2 &&
				i > j)
			{
				w = b1;
				h = b2;

				if (w < 0 || (!w && render_opts[0] == '-'))
				{
					w *= (w_sgn = -1);
				}
				if (h < 0 || (!h && render_opts[j] == '-'))
				{
					h *= (h_sgn = -1);
				}
			}
			else if (
				sscanf(render_opts, "%d%d%n", &b1, &b2, &i) >=
				2)
			{
				dw += b1;
				dh += b2;
			}
		}
		render_opts += i ? i : 1;
	}
	free(allocated_path);

	/* Keep the original aspect ratio when either w or h is 0 */
        Frsvg_handle_get_dimensions(rsvg, &dim);
	if (!w && !h)
	{
		w = dim.width;
		h = dim.height;
	}
	else if (!w)
	{
		w = h * dim.em / dim.ex;
	}
	else if (!h)
	{
		h = w * dim.ex / dim.em;
	}

	data = (CARD32 *)safemalloc(w * h * sizeof(CARD32));
	memset(data, 0, w * h * sizeof(CARD32));
	surface = Fcairo_image_surface_create_for_data((unsigned char *)data,
		FCAIRO_FORMAT_ARGB32, w, h, w * sizeof(CARD32));
	if (Fcairo_surface_status(surface) != FCAIRO_STATUS_SUCCESS)
	{
		Fg_object_unref(FG_OBJECT(rsvg));
		free(data);
		if (surface)
		{
		   	Fcairo_surface_destroy(surface);
		}

		return False;
	}
	cr = Fcairo_create(surface);
	Fcairo_surface_destroy(surface);
	if (Fcairo_status(cr) != FCAIRO_STATUS_SUCCESS)
	{
		Fg_object_unref(FG_OBJECT(rsvg));
		free(data);
		if (cr)
		{
			Fcairo_destroy(cr);
		}

		return False;
	}

	/* Affine transformations ...
	 * mirroring, rotation, scaling and translation */
	Fcairo_translate(cr, .5 * w + dw, .5 * h + dh);
	Fcairo_scale(cr, scale, scale);
	Fcairo_rotate(cr, angle);
	Fcairo_translate(cr, -.5 * w * w_sgn, -.5 * h * h_sgn);
	Fcairo_scale(cr, w * w_sgn / dim.em, h * h_sgn / dim.ex);

	Frsvg_handle_render_cairo(rsvg, cr);
	Fg_object_unref(FG_OBJECT(rsvg));
	Fcairo_destroy(cr);

	/* Cairo gave us alpha prescaled RGB values, hence we need
	 * to rescale them for PImageCreatePixmapFromArgbData() */
	for (i = 0; i < w * h; i++)
	{
		if ((a_value = (data[i] >> 030) & 0xff))
		{
			r_value = ((data[i] >> 020) & 0xff) * 0xff / a_value;
			g_value = ((data[i] >> 010) & 0xff) * 0xff / a_value;
			b_value =  (data[i]         & 0xff) * 0xff / a_value;

			data[i] =
				(a_value << 030) | (r_value << 020) |
				(g_value << 010) | b_value;
		}
	}

	*width = w;
	*height = h;
	*argb_data = data;

	return True;
}

/*
 *
 * png loader
 *
 */
static
Bool PImageLoadPng(FIMAGE_CMD_ARGS)
{
	Fpng_uint_32 w32, h32;
	Fpng_structp Fpng_ptr = NULL;
	Fpng_infop Finfo_ptr = NULL;
	CARD32 *data;
	int w, h;
	char hasa = 0, hasg = 0;
	FILE *f;
	int bit_depth, color_type, interlace_type;
	unsigned char buf[FPNG_BYTES_TO_CHECK];
	unsigned char **lines;
	int i;

	if (!PngSupport)
		return False;

	if (!(f = fopen(path, "rb")))
	{
		return False;
	}

	fread(buf, 1, FPNG_BYTES_TO_CHECK, f);
	if (!Fpng_check_sig(buf, FPNG_BYTES_TO_CHECK))
	{
		fclose(f);
		return False;
	}
	rewind(f);
	Fpng_ptr = Fpng_create_read_struct(FPNG_LIBPNG_VER_STRING,
					   NULL, NULL, NULL);
	if (!Fpng_ptr)
	{
		fclose(f);
		return False;
	}
	Finfo_ptr = Fpng_create_info_struct(Fpng_ptr);
	if (!Finfo_ptr)
	{
		Fpng_destroy_read_struct(&Fpng_ptr, NULL, NULL);
		fclose(f);
		return False;
	}
#if 0
	if (setjmp(Fpng_ptr->jmpbuf))
	{
		Fpng_destroy_read_struct(&Fpng_ptr, &Finfo_ptr, NULL);
		fclose(f);
		return False;
	}
#endif
	Fpng_init_io(Fpng_ptr, f);
	Fpng_read_info(Fpng_ptr, Finfo_ptr);
	Fpng_get_IHDR(Fpng_ptr, Finfo_ptr, (Fpng_uint_32 *) (&w32),
		     (Fpng_uint_32 *) (&h32), &bit_depth, &color_type,
		     &interlace_type, NULL, NULL);
	interlace_type = 0; /* not used */
	*width = w = (int) w32;
	*height = h = (int) h32;
	if (color_type == FPNG_COLOR_TYPE_PALETTE)
	{
		Fpng_set_expand(Fpng_ptr);
	}
	if (Finfo_ptr->color_type == FPNG_COLOR_TYPE_RGB_ALPHA)
	{
		hasa = 1;
	}
	if (Finfo_ptr->color_type == FPNG_COLOR_TYPE_GRAY_ALPHA)
	{
		hasa = 1;
		hasg = 1;
	}
	if (Finfo_ptr->color_type == FPNG_COLOR_TYPE_GRAY)
	{
		hasg = 1;
	}
	if (hasa)
		Fpng_set_expand(Fpng_ptr);
	/* we want ARGB */
	/* note form raster:
	* thanks to mustapha for helping debug this on PPC Linux remotely by
	* sending across screenshots all the tiem and me figuring out form them
	* what the hell was up with the colors
	* now png loading shoudl work on big endian machines nicely   */
#ifdef WORDS_BIGENDIAN
	Fpng_set_swap_alpha(Fpng_ptr);
	Fpng_set_filler(Fpng_ptr, 0xff, FPNG_FILLER_BEFORE);
#else
	Fpng_set_bgr(Fpng_ptr);
	Fpng_set_filler(Fpng_ptr, 0xff, FPNG_FILLER_AFTER);
#endif
	/* 16bit color -> 8bit color */
	Fpng_set_strip_16(Fpng_ptr);
	/* pack all pixels to byte boundaires */
	Fpng_set_packing(Fpng_ptr);
	if (Fpng_get_valid(Fpng_ptr, Finfo_ptr, FPNG_INFO_tRNS))
		Fpng_set_expand(Fpng_ptr);

	data = (CARD32 *)safemalloc(w * h * sizeof(CARD32));
	lines = (unsigned char **) safemalloc(h * sizeof(unsigned char *));

	if (hasg)
	{
		Fpng_set_gray_to_rgb(Fpng_ptr);
		if (Fpng_get_bit_depth(Fpng_ptr, Finfo_ptr) < 8)
		{
			Fpng_set_gray_1_2_4_to_8(Fpng_ptr);
		}
	}
	for (i = 0; i < h; i++)
	{
		lines[i] =((unsigned char *) (data)) + (i * w * sizeof(CARD32));
	}
	Fpng_read_image(Fpng_ptr, lines);
	Fpng_read_end(Fpng_ptr, Finfo_ptr);
	Fpng_destroy_read_struct(&Fpng_ptr, &Finfo_ptr, (png_infopp) NULL);
	fclose(f);
	free(lines);
	*argb_data = data;

	return True;
}

/*
 *
 * xpm loader
 *
 */
static
Bool PImageLoadXpm(FIMAGE_CMD_ARGS)
{
	FxpmImage xpm_im = {0};
	FxpmColor *xpm_color;
	XColor color;
	CARD32 *colors;
	CARD32 *data;
	char *visual_color;
	int i;
	int w;
	int h;
	int rc;
#ifdef HAVE_SIGACTION
	struct sigaction defaultHandler;
	struct sigaction originalHandler;
#else
	RETSIGTYPE (*originalHandler)(int);
#endif

	if (!XpmSupport)
		return False;

#ifdef HAVE_SIGACTION
	sigemptyset(&defaultHandler.sa_mask);
	defaultHandler.sa_flags = 0;
	defaultHandler.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &defaultHandler, &originalHandler);
#else
	originalHandler = signal(SIGCHLD, SIG_DFL);
#endif


	rc = FxpmReadFileToXpmImage(path, &xpm_im, NULL);

#ifdef HAVE_SIGACTION
	sigaction(SIGCHLD, &originalHandler, NULL);
#else
	signal(SIGCHLD, originalHandler);
#endif

	if (rc != FxpmSuccess)
	{
		return False;
	}

	if (xpm_im.ncolors <= 0)
	{
		FxpmFreeXpmImage(&xpm_im);
		return False;
	}
	colors = (CARD32 *)safemalloc(xpm_im.ncolors * sizeof(CARD32));
	for(i=0; i < xpm_im.ncolors; i++)
	{
		xpm_color = &xpm_im.colorTable[i];
		if (xpm_color->c_color)
		{
			visual_color = xpm_color->c_color;
		}
		else if (xpm_color->g_color)
		{
			visual_color = xpm_color->g_color;
		}
		else if (xpm_color->g4_color)
		{
			visual_color = xpm_color->g4_color;
		}
		else
		{
			visual_color = xpm_color->m_color;
		}
		if (XParseColor(dpy, Pcmap, visual_color, &color))
		{
			colors[i] = 0xff000000 |
				((color.red  << 8) & 0xff0000) |
				((color.green    ) & 0xff00) |
				((color.blue >> 8) & 0xff);
		}
		else
		{
			colors[i] = 0;
		}
	}
	*width = w = xpm_im.width;
	*height = h = xpm_im.height;
	data = (CARD32 *)safemalloc(w * h * sizeof(CARD32));
	for (i=0; i < w * h; i++)
	{
		data[i] = colors[xpm_im.data[i]];
	}
	free(colors);
	*argb_data = data;

	return True;
}

/*
 *
 * copy image to server
 *
 */
static
Pixmap PImageCreatePixmapFromFImage(Display *dpy, Window win, FImage *fimage)
{
	GC gc;
	Pixmap pixmap;
	int w;
	int h;
	int depth;
	int must_free_gc;

	w = fimage->im->width;
	h = fimage->im->height;
	depth = fimage->im->depth;
	pixmap = XCreatePixmap(dpy, win, w, h, depth);
	if (depth == Pdepth)
	{
		gc = PictureDefaultGC(dpy, win);
		must_free_gc = 0;
	}
	else
	{
		gc = fvwmlib_XCreateGC(dpy, pixmap, 0, NULL);
		must_free_gc = 1;
	}
	FPutFImage(dpy, pixmap, gc, fimage, 0, 0, 0, 0, w, h);
	if (depth != Pdepth)
	{
		XFreeGC(dpy, gc);
	}

	return pixmap;
}

/* ---------------------------- interface functions ------------------------ */

/*
 *
 * argb data to pixmaps
 *
 */
Bool PImageCreatePixmapFromArgbData(
	Display *dpy, Window win, CARD32 *data, int start, int width,
	int height, Pixmap *pixmap, Pixmap *mask, Pixmap *alpha,
	int *nalloc_pixels, Pixel **alloc_pixels, int *no_limit,
	FvwmPictureAttributes fpa)
{
	FImage *fim;
	FImage *m_fim = NULL;
	FImage *a_fim = NULL;
	XColor c;
	int i;
	int j;
	int a;
	PictureImageColorAllocator *pica;
	int alpha_limit = PICTURE_ALPHA_LIMIT;
	int alpha_depth = FRenderGetAlphaDepth();
	Bool have_mask = False;
	Bool have_alpha = False;


	fim = FCreateFImage(
		dpy, Pvisual, Pdepth, ZPixmap, width, height);
	if (!fim)
	{
		return False;
	}
	if (mask)
	{
		m_fim = FCreateFImage(
			dpy, Pvisual, 1, ZPixmap, width, height);
	}
	if (alpha && !(fpa.mask & FPAM_NO_ALPHA) && alpha_depth)
	{
		alpha_limit = 0;
		a_fim = FCreateFImage(
			dpy, Pvisual, alpha_depth, ZPixmap, width, height);
	}

	pica = PictureOpenImageColorAllocator(
		dpy, Pcmap, width, height,
		!!(fpa.mask & FPAM_NO_COLOR_LIMIT),
		!!(fpa.mask & FPAM_NO_ALLOC_PIXELS),
		!!(fpa.mask & FPAM_DITHER),
		True);

	data += start;
	c.flags = DoRed | DoGreen | DoBlue;
	for (j = 0; j < height; j++)
	{
		for (i = 0; i < width; i++, data++)
		{
			a = (*data >> 030) & 0xff;
			if (a > alpha_limit)
			{
				c.red   = (*data >> 16) & 0xff;
				c.green = (*data >>  8) & 0xff;
				c.blue  = (*data      ) & 0xff;

				PictureAllocColorImage(
					dpy, pica, &c, i, j);
				XPutPixel(fim->im, i, j, c.pixel);

				if (m_fim)
				{
					XPutPixel(m_fim->im, i, j, 1);
				}
			}
			else if (m_fim != NULL)
			{
				XPutPixel(m_fim->im, i, j, 0);
				have_mask = True;
			}
			if (a_fim != NULL)
			{
				XPutPixel(a_fim->im, i, j, a);
				if (a > 0 && a < 0xff)
				{
					have_alpha = True;
				}
			}
		}
	}
	/* copy the image to the server */
	*pixmap = PImageCreatePixmapFromFImage(dpy, win, fim);
	if (have_alpha)
	{
		*alpha = PImageCreatePixmapFromFImage(dpy, win, a_fim);
	}
	else if (have_mask)
	{
		*mask = PImageCreatePixmapFromFImage(dpy, win, m_fim);
	}
	FDestroyFImage(dpy, fim);
	if (m_fim)
	{
		FDestroyFImage(dpy, m_fim);
	}
	if (a_fim)
	{
		FDestroyFImage(dpy, a_fim);
	}
	PictureCloseImageColorAllocator(
		dpy, pica, nalloc_pixels, alloc_pixels, no_limit);
	return True;
}


/*
 *
 * the images loaders
 *
 */

Bool PImageLoadPixmapFromFile(
	Display *dpy, Window win, char *path, Pixmap *pixmap, Pixmap *mask,
	Pixmap *alpha, int *width, int *height, int *depth,
	int *nalloc_pixels, Pixel **alloc_pixels,
	int *no_limit, FvwmPictureAttributes fpa)
{
	CARD32 *data;

	if(PImageLoadArgbDataFromFile(dpy, path, &data, width, height))
	{
		*depth = Pdepth;
		if (PImageCreatePixmapFromArgbData(
			dpy, win, data, 0, *width, *height, pixmap, mask,
			alpha, nalloc_pixels, alloc_pixels, no_limit, fpa))
		{
			free(data);

			return True;
		}
		free(data);
	}
	/* Bitmap fallback */
	else if (XReadBitmapFile(
			dpy, win, path, (unsigned int *)width,
			(unsigned int *)height, pixmap, NULL, NULL)
		 == BitmapSuccess)
	{
		*depth = 1;
		*mask = None;

		return True;
	}
	pixmap = None;
	mask = None;
	alpha = None;
	*width = *height = *depth = 0;
	if (nalloc_pixels != NULL)
	{
		*nalloc_pixels = 0;
	}
	if (alloc_pixels != NULL)
	{
		*alloc_pixels = NULL;
	}
	return False;
}

FvwmPicture *PImageLoadFvwmPictureFromFile(
	Display *dpy, Window win, char *path, FvwmPictureAttributes fpa)
{
	FvwmPicture *p;
	Pixmap pixmap = None;
	Pixmap mask = None;
	Pixmap alpha = None;
	int width = 0, height = 0;
	int depth = 0, no_limit;
	int nalloc_pixels = 0;
	Pixel *alloc_pixels = NULL;
	char *real_path;

        /* Remove any svg rendering options from real_path */
	if (USE_SVG && *path == ':' &&
	    (real_path = strchr(path + 1, ':')))
	{
		real_path ++;
	}
	else
	{
		real_path = path;
	}
	if (!PImageLoadPixmapFromFile(
		dpy, win, path, &pixmap, &mask, &alpha, &width, &height,
		&depth, &nalloc_pixels, &alloc_pixels, &no_limit, fpa))
	{
		return NULL;
	}

	p = (FvwmPicture*)safemalloc(sizeof(FvwmPicture));
	memset(p, 0, sizeof(FvwmPicture));
	p->count = 1;
	p->name = path;
	p->fpa_mask = fpa.mask;
	p->next = NULL;
	setFileStamp(&p->stamp, real_path);
	p->picture = pixmap;
	p->mask = mask;
	p->alpha = alpha;
	p->width = width;
	p->height = height;
	p->depth = depth;
	p->nalloc_pixels = nalloc_pixels;
	p->alloc_pixels = alloc_pixels;
	p->no_limit = no_limit;
	return p;
}


Bool PImageLoadCursorPixmapFromFile(
	Display *dpy, Window win, char *path, Pixmap *source, Pixmap *mask,
	int *x, int *y)
{

	FxpmAttributes xpm_attributes;

	if (!XpmSupport)
		return False;

	/* we need source to be a bitmap */
	xpm_attributes.depth = 1;
	xpm_attributes.valuemask = FxpmSize | FxpmDepth | FxpmHotspot;
	if (FxpmReadFileToPixmap(dpy, win, path, source, mask,
				&xpm_attributes) != FxpmSuccess)
	{
		fprintf(stderr, "[FVWM][PImageLoadCursorPixmapFromFile]"
			" Error reading cursor xpm %s",
			path);
		return False;
	}

	*x = xpm_attributes.x_hotspot;
	if (*x >= xpm_attributes.width)
	{
		*x = xpm_attributes.width / 2;
	}
	*y = xpm_attributes.y_hotspot;
	if (*y >= xpm_attributes.height)
	{
		*y = xpm_attributes.height / 2;
	}
	return True;
}

/* FIXME: Use color limit */
Bool PImageLoadPixmapFromXpmData(
	Display *dpy, Window win, int color_limit,
	char **data,
	Pixmap *pixmap, Pixmap *mask,
	int *width, int *height, int *depth)
{
	FxpmAttributes xpm_attributes;

	xpm_attributes.valuemask = FxpmCloseness |
		FxpmExtensions | FxpmVisual | FxpmColormap | FxpmDepth;
	xpm_attributes.closeness = 40000;
	xpm_attributes.visual = Pvisual;
	xpm_attributes.colormap = Pcmap;
	xpm_attributes.depth = Pdepth;
	if(FxpmCreatePixmapFromData(dpy, win, data, pixmap, mask,
				   &xpm_attributes)!=FxpmSuccess)
	{
		return False;
	}
	*width = xpm_attributes.width;
	*height = xpm_attributes.height;
	*depth = Pdepth;
	return True;
}
