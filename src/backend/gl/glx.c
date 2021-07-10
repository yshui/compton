// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * Copyright (c) 2019 Yuxuan Shui <yshuiv7@gmail.com>
 * See LICENSE-mit for more information.
 *
 */

#include <X11/Xlib-xcb.h>
#include <assert.h>
#include <limits.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "backend/gl/gl_common.h"
#include "backend/gl/glx.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "utils.h"
#include "win.h"
#include "x.h"

struct _glx_pixmap {
	GLXPixmap glpixmap;
	xcb_pixmap_t pixmap;
	bool owned;
};

struct _glx_data {
	struct gl_data gl;
	Display *display;
	int screen;
	xcb_window_t target_win;
	GLXContext ctx;
};

#define glXGetFBConfigAttribChecked(a, b, attr, c)                                       \
	do {                                                                             \
		if (glXGetFBConfigAttrib(a, b, attr, c)) {                               \
			log_info("Cannot get FBConfig attribute " #attr);                \
			continue;                                                        \
		}                                                                        \
	} while (0)

struct glx_fbconfig_info *glx_find_fbconfig(Display *dpy, int screen, struct xvisual_info m) {
	log_debug("Looking for FBConfig for RGBA%d%d%d%d, depth %d", m.red_size,
	          m.blue_size, m.green_size, m.alpha_size, m.visual_depth);

	int ncfg;
	// clang-format off
	GLXFBConfig *cfg =
	    glXChooseFBConfig(dpy, screen, (int[]){
	                          GLX_RENDER_TYPE, GLX_RGBA_BIT,
	                          GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
	                          GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
	                          GLX_X_RENDERABLE, true,
	                          GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT, (GLint)GLX_DONT_CARE,
	                          GLX_BUFFER_SIZE, m.red_size + m.green_size +
	                                           m.blue_size + m.alpha_size,
	                          GLX_RED_SIZE, m.red_size,
	                          GLX_BLUE_SIZE, m.blue_size,
	                          GLX_GREEN_SIZE, m.green_size,
	                          GLX_ALPHA_SIZE, m.alpha_size,
	                          GLX_STENCIL_SIZE, 0,
	                          GLX_DEPTH_SIZE, 0,
	                          0
	                      }, &ncfg);
	// clang-format on

	int texture_tgts, y_inverted, texture_fmt;
	bool found = false;
	int min_cost = INT_MAX;
	GLXFBConfig ret;
	for (int i = 0; i < ncfg; i++) {
		int depthbuf, stencil, doublebuf, bufsize;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BUFFER_SIZE, &bufsize);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_DEPTH_SIZE, &depthbuf);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_STENCIL_SIZE, &stencil);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_DOUBLEBUFFER, &doublebuf);
		if (depthbuf + stencil + bufsize * (doublebuf + 1) >= min_cost) {
			continue;
		}
		int red, green, blue;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_RED_SIZE, &red);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BLUE_SIZE, &blue);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_GREEN_SIZE, &green);
		if (red != m.red_size || green != m.green_size || blue != m.blue_size) {
			// Color size doesn't match, this cannot work
			continue;
		}

		int rgb, rgba;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &rgba);
		if (!rgb && !rgba) {
			log_info("FBConfig is neither RGBA nor RGB, we cannot "
			         "handle this setup.");
			continue;
		}

		int visual;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_VISUAL_ID, &visual);
		if (m.visual_depth != -1 &&
		    x_get_visual_depth(XGetXCBConnection(dpy), (xcb_visualid_t)visual) !=
		        m.visual_depth) {
			// FBConfig and the correspondent X Visual might not have the same
			// depth. (e.g. 32 bit FBConfig with a 24 bit Visual). This is
			// quite common, seen in both open source and proprietary drivers.
			//
			// If the FBConfig has a matching depth but its visual doesn't, we
			// still cannot use it.
			continue;
		}

		// All check passed, we are using this one.
		found = true;
		ret = cfg[i];
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT,
		                            &texture_tgts);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_Y_INVERTED_EXT, &y_inverted);

		// Prefer the texture format with matching alpha, with the other one as
		// fallback
		if (m.alpha_size) {
			texture_fmt = rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT
			                   : GLX_TEXTURE_FORMAT_RGB_EXT;
		} else {
			texture_fmt =
			    rgb ? GLX_TEXTURE_FORMAT_RGB_EXT : GLX_TEXTURE_FORMAT_RGBA_EXT;
		}
		min_cost = depthbuf + stencil + bufsize * (doublebuf + 1);
	}
	free(cfg);
	if (!found) {
		return NULL;
	}

	auto info = cmalloc(struct glx_fbconfig_info);
	info->cfg = ret;
	info->texture_tgts = texture_tgts;
	info->texture_fmt = texture_fmt;
	info->y_inverted = y_inverted;
	return info;
}

/**
 * Free a glx_texture_t.
 */
static void glx_release_image(backend_t *base, struct gl_texture *tex) {
	struct _glx_data *gd = (void *)base;

	struct _glx_pixmap *p = tex->user_data;
	// Release binding
	if (p->glpixmap && tex->texture) {
		glBindTexture(GL_TEXTURE_2D, tex->texture);
		glXReleaseTexImageEXT(gd->display, p->glpixmap, GLX_FRONT_LEFT_EXT);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// Free GLX Pixmap
	if (p->glpixmap) {
		glXDestroyPixmap(gd->display, p->glpixmap);
		p->glpixmap = 0;
	}

	if (p->owned) {
		xcb_free_pixmap(base->c, p->pixmap);
		p->pixmap = XCB_NONE;
	}

	free(p);
	tex->user_data = NULL;
}

/**
 * Destroy GLX related resources.
 */
void glx_deinit(backend_t *base) {
	struct _glx_data *gd = (void *)base;

	gl_deinit(&gd->gl);

	// Destroy GLX context
	if (gd->ctx) {
		glXMakeCurrent(gd->display, None, NULL);
		glXDestroyContext(gd->display, gd->ctx);
		gd->ctx = 0;
	}

	free(gd);
}

static void *glx_decouple_user_data(backend_t *base attr_unused, void *ud attr_unused) {
	auto ret = cmalloc(struct _glx_pixmap);
	ret->owned = false;
	ret->glpixmap = 0;
	ret->pixmap = 0;
	return ret;
}

static int glx_get_swap_interval(int interval, Display *dpy, GLXDrawable drawable) {
	if (glxext.has_GLX_EXT_swap_control) {
		unsigned int val;
		glXQueryDrawable(dpy, drawable, GLX_SWAP_INTERVAL_EXT, &val);
		interval = glxext.has_GLX_EXT_swap_control_tear ? -((int)val) : (int)val;
	}
	else if(glxext.has_GLX_MESA_swap_control) {
		interval = glXGetSwapIntervalMESA();
	}
	return interval;
}

static bool glx_set_swap_interval(int interval, Display *dpy, GLXDrawable drawable, bool is_nvidia) {
	bool vsync_enabled = false;
	if (glxext.has_GLX_EXT_swap_control) {
		if(is_nvidia) {
			/* See: https://git.sailfishos.org/mer-core/libsdl/commit/771a34fc63f9dd6e2bbe65d17c18882e5c87a6be?view=parallel&w=1 */
			glXSwapIntervalEXT(dpy, drawable, glx_get_swap_interval(interval, dpy, drawable));
		}
		glXSwapIntervalEXT(dpy, drawable, glxext.has_GLX_EXT_swap_control_tear ? -interval : interval);
		vsync_enabled = (interval != 0); // glXSwapIntervalEXT doesn't return if it's successful
	}
	else if (glxext.has_GLX_MESA_swap_control && (glXSwapIntervalMESA((uint)interval) == 0)) {
		vsync_enabled = (interval != 0);
	}
	else if (glxext.has_GLX_SGI_swap_control && (glXSwapIntervalSGI(interval) == 0)) {
		vsync_enabled = (interval != 0);
	}
	return vsync_enabled;
}

/**
 * Initialize OpenGL.
 */
static backend_t *glx_init(session_t *ps) {
	bool success = false;
	glxext_init(ps->dpy, ps->scr);
	auto gd = ccalloc(1, struct _glx_data);
	init_backend_base(&gd->gl.base, ps);

	gd->display = ps->dpy;
	gd->screen = ps->scr;
	gd->target_win = session_get_target_window(ps);

	XVisualInfo *pvis = NULL;

	// Check for GLX extension
	if (!ps->glx_exists) {
		log_error("No GLX extension.");
		goto end;
	}

	// Get XVisualInfo
	int nitems = 0;
	XVisualInfo vreq = {.visualid = ps->vis};
	pvis = XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
	if (!pvis) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		goto end;
	}

	// Ensure the visual is double-buffered
	int value = 0;
	if (glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
		log_error("Root visual is not a GL visual.");
		goto end;
	}

	if (glXGetConfig(ps->dpy, pvis, GLX_STENCIL_SIZE, &value) || !value) {
		log_error("Root visual lacks stencil buffer.");
		goto end;
	}

	if (glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value) || !value) {
		log_error("Root visual is not a double buffered GL visual.");
		goto end;
	}

	if (glXGetConfig(ps->dpy, pvis, GLX_RGBA, &value) || !value) {
		log_error("Root visual is a color index visual, not supported");
		goto end;
	}

	if (!glxext.has_GLX_EXT_texture_from_pixmap) {
		log_error("GLX_EXT_texture_from_pixmap is not supported by your driver");
		goto end;
	}

	if (!glxext.has_GLX_ARB_create_context) {
		log_error("GLX_ARB_create_context is not supported by your driver");
		goto end;
	}

	// Find a fbconfig with visualid matching the one from the target win, so we can
	// be sure that the fbconfig is compatible with our target window.
	int ncfgs;
	GLXFBConfig *cfg = glXGetFBConfigs(gd->display, gd->screen, &ncfgs);
	bool found = false;
	for (int i = 0; i < ncfgs; i++) {
		int visualid;
		glXGetFBConfigAttribChecked(gd->display, cfg[i], GLX_VISUAL_ID, &visualid);
		if ((VisualID)visualid != pvis->visualid) {
			continue;
		}

		gd->ctx = glXCreateContextAttribsARB(ps->dpy, cfg[i], 0, true,
		                                     (int[]){
		                                         GLX_CONTEXT_MAJOR_VERSION_ARB,
		                                         3,
		                                         GLX_CONTEXT_MINOR_VERSION_ARB,
		                                         3,
		                                         GLX_CONTEXT_PROFILE_MASK_ARB,
		                                         GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
		                                         0,
		                                     });
		free(cfg);

		if (!gd->ctx) {
			log_error("Failed to get GLX context.");
			goto end;
		}
		found = true;
		break;
	}

	if (!found) {
		log_error("Couldn't find a suitable fbconfig for the target window");
		goto end;
	}

	// Attach GLX context
	GLXDrawable tgt = gd->target_win;
	if (!glXMakeCurrent(ps->dpy, tgt, gd->ctx)) {
		log_error("Failed to attach GLX context.");
		goto end;
	}

	if (!gl_init(&gd->gl, ps)) {
		log_error("Failed to setup OpenGL");
		goto end;
	}

	gd->gl.decouple_texture_user_data = glx_decouple_user_data;
	gd->gl.release_user_data = glx_release_image;

	if (ps->o.vsync) {
		if (!glx_set_swap_interval(1, ps->dpy, tgt, gd->gl.is_nvidia)) {
			log_error("Failed to enable vsync.");
		}
	} else {
		glx_set_swap_interval(0, ps->dpy, tgt, gd->gl.is_nvidia);
	}

	success = true;

end:
	if (pvis) {
		XFree(pvis);
	}

	if (!success) {
		glx_deinit(&gd->gl.base);
		return NULL;
	}

	return &gd->gl.base;
}

static void *
glx_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap, struct xvisual_info fmt, bool owned) {
	struct _glx_data *gd = (void *)base;
	struct _glx_pixmap *glxpixmap = NULL;
	// Retrieve pixmap parameters, if they aren't provided
	if (fmt.visual_depth > OPENGL_MAX_DEPTH) {
		log_error("Requested depth %d higher than max possible depth %d.",
		          fmt.visual_depth, OPENGL_MAX_DEPTH);
		return false;
	}

	if (fmt.visual_depth < 0) {
		log_error("Pixmap %#010x with invalid depth %d", pixmap, fmt.visual_depth);
		return false;
	}

	auto r = xcb_get_geometry_reply(base->c, xcb_get_geometry(base->c, pixmap), NULL);
	if (!r) {
		log_error("Invalid pixmap %#010x", pixmap);
		return NULL;
	}

	log_trace("Binding pixmap %#010x", pixmap);
	auto wd = ccalloc(1, struct backend_image);
	wd->max_brightness = 1;
	auto inner = ccalloc(1, struct gl_texture);
	inner->width = wd->ewidth = r->width;
	inner->height = wd->eheight = r->height;
	wd->inner = (struct backend_image_inner_base *)inner;
	free(r);

	auto fbcfg = glx_find_fbconfig(gd->display, gd->screen, fmt);
	if (!fbcfg) {
		log_error("Couldn't find FBConfig with requested visual %x", fmt.visual);
		goto err;
	}

	// Choose a suitable texture target for our pixmap.
	// Refer to GLX_EXT_texture_om_pixmap spec to see what are the mean
	// of the bits in texture_tgts
	if (!(fbcfg->texture_tgts & GLX_TEXTURE_2D_BIT_EXT)) {
		log_error("Cannot bind pixmap to GL_TEXTURE_2D, giving up");
		goto err;
	}

	log_debug("depth %d, rgba %d", fmt.visual_depth,
	          (fbcfg->texture_fmt == GLX_TEXTURE_FORMAT_RGBA_EXT));

	GLint attrs[] = {
	    GLX_TEXTURE_FORMAT_EXT,
	    fbcfg->texture_fmt,
	    GLX_TEXTURE_TARGET_EXT,
	    GLX_TEXTURE_2D_EXT,
	    0,
	};

	inner->y_inverted = fbcfg->y_inverted;

	glxpixmap = cmalloc(struct _glx_pixmap);
	glxpixmap->pixmap = pixmap;
	glxpixmap->glpixmap = glXCreatePixmap(gd->display, fbcfg->cfg, pixmap, attrs);
	glxpixmap->owned = owned;
	free(fbcfg);

	if (!glxpixmap->glpixmap) {
		log_error("Failed to create glpixmap for pixmap %#010x", pixmap);
		goto err;
	}

	log_trace("GLXPixmap %#010lx", glxpixmap->glpixmap);

	// Create texture
	inner->user_data = glxpixmap;
	inner->texture = gl_new_texture(GL_TEXTURE_2D);
	inner->has_alpha = fmt.alpha_size != 0;
	wd->opacity = 1;
	wd->color_inverted = false;
	wd->dim = 0;
	wd->inner->refcount = 1;
	glBindTexture(GL_TEXTURE_2D, inner->texture);
	glXBindTexImageEXT(gd->display, glxpixmap->glpixmap, GLX_FRONT_LEFT_EXT, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_check_err();
	return wd;
err:
	if (glxpixmap && glxpixmap->glpixmap) {
		glXDestroyPixmap(gd->display, glxpixmap->glpixmap);
	}
	free(glxpixmap);

	if (owned) {
		xcb_free_pixmap(base->c, pixmap);
	}
	free(wd);
	return NULL;
}

static void glx_present(backend_t *base, const region_t *region attr_unused) {
	struct _glx_data *gd = (void *)base;
	gl_present(base, region);
	glXSwapBuffers(gd->display, gd->target_win);
	if(gd->gl.use_glfinish)
	{
		if (gd->gl.is_nvidia)
			glFlush();
		else
			glFinish();
	}
}

static int glx_buffer_age(backend_t *base) {
	if (!glxext.has_GLX_EXT_buffer_age) {
		return -1;
	}

	struct _glx_data *gd = (void *)base;
	unsigned int val;
	glXQueryDrawable(gd->display, gd->target_win, GLX_BACK_BUFFER_AGE_EXT, &val);
	return (int)val ?: -1;
}

static void glx_diagnostics(backend_t *base) {
	struct _glx_data *gd = (void *)base;
	bool warn_software_rendering = false;
	const char *software_renderer_names[] = {"llvmpipe", "SWR", "softpipe"};
	auto glx_vendor = glXGetClientString(gd->display, GLX_VENDOR);
	printf("* Driver vendors:\n");
	printf(" * GLX: %s\n", glx_vendor);
	printf(" * GL: %s\n", glGetString(GL_VENDOR));

	auto gl_renderer = (const char *)glGetString(GL_RENDERER);
	printf("* GL renderer: %s\n", gl_renderer);
	if (strcmp(glx_vendor, "Mesa Project and SGI")) {
		for (size_t i = 0; i < ARR_SIZE(software_renderer_names); i++) {
			if (strstr(gl_renderer, software_renderer_names[i]) != NULL) {
				warn_software_rendering = true;
				break;
			}
		}
	}

#ifdef GLX_MESA_query_renderer
	if (glxext.has_GLX_MESA_query_renderer) {
		unsigned int accelerated = 0;
		glXQueryCurrentRendererIntegerMESA(GLX_RENDERER_ACCELERATED_MESA, &accelerated);
		printf("* Accelerated: %d\n", accelerated);

		// Trust GLX_MESA_query_renderer when it's available
		warn_software_rendering = (accelerated == 0);
	}
#endif

	if (warn_software_rendering) {
		printf("\n(You are using a software renderer. Unless you are doing this\n"
		       "intentionally, this means you don't have a graphics driver\n"
		       "properly installed. Performance will suffer. Please fix this\n"
		       "before reporting your issue.)\n");
	}
}

struct backend_operations glx_ops = {
    .init = glx_init,
    .deinit = glx_deinit,
    .bind_pixmap = glx_bind_pixmap,
    .release_image = gl_release_image,
    .compose = gl_compose,
    .image_op = gl_image_op,
    .set_image_property = default_set_image_property,
    .read_pixel = gl_read_pixel,
    .clone_image = default_clone_image,
    .blur = gl_blur,
    .is_image_transparent = default_is_image_transparent,
    .present = glx_present,
    .buffer_age = glx_buffer_age,
    .render_shadow = default_backend_render_shadow,
    .fill = gl_fill,
    .create_blur_context = gl_create_blur_context,
    .destroy_blur_context = gl_destroy_blur_context,
    .get_blur_size = gl_get_blur_size,
    .diagnostics = glx_diagnostics,
    .max_buffer_age = 5,        // Why?
};

/**
 * Check if a GLX extension exists.
 */
static inline bool glx_has_extension(Display *dpy, int screen, const char *ext) {
	const char *glx_exts = glXQueryExtensionsString(dpy, screen);
	if (!glx_exts) {
		log_error("Failed get GLX extension list.");
		return false;
	}

	auto inlen = strlen(ext);
	const char *curr = glx_exts;
	bool match = false;
	while (curr && !match) {
		const char *end = strchr(curr, ' ');
		if (!end) {
			// Last extension string
			match = strcmp(ext, curr) == 0;
		} else if (curr + inlen == end) {
			// Length match, do match string
			match = strncmp(ext, curr, (unsigned long)(end - curr)) == 0;
		}
		curr = end ? end + 1 : NULL;
	}

	if (!match) {
		log_info("Missing GLX extension %s.", ext);
	} else {
		log_info("Found GLX extension %s.", ext);
	}

	return match;
}

struct glxext_info glxext = {0};
PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI;
PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI;
PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML;
PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML;
PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
PFNGLXGETSWAPINTERVALMESAPROC glXGetSwapIntervalMESA;
PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT;
PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;
PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB;

#ifdef GLX_MESA_query_renderer
PFNGLXQUERYCURRENTRENDERERINTEGERMESAPROC glXQueryCurrentRendererIntegerMESA;
#endif

void glxext_init(Display *dpy, int screen) {
	if (glxext.initialized) {
		return;
	}
	glxext.initialized = true;
#define check_ext(name) glxext.has_##name = glx_has_extension(dpy, screen, #name)
	check_ext(GLX_SGI_video_sync);
	check_ext(GLX_SGI_swap_control);
	check_ext(GLX_OML_sync_control);
	check_ext(GLX_MESA_swap_control);
	check_ext(GLX_EXT_swap_control);
	check_ext(GLX_EXT_swap_control_tear);
	check_ext(GLX_EXT_texture_from_pixmap);
	check_ext(GLX_ARB_create_context);
	check_ext(GLX_EXT_buffer_age);
#ifdef GLX_MESA_query_renderer
	check_ext(GLX_MESA_query_renderer);
#endif
#undef check_ext

#define lookup(name) (name = (__typeof__(name))glXGetProcAddress((GLubyte *)#name))
	// Checking if the returned function pointer is NULL is not really necessary,
	// or maybe not even useful, since glXGetProcAddress might always return
	// something. We are doing it just for completeness' sake.
	if (!lookup(glXGetVideoSyncSGI) || !lookup(glXWaitVideoSyncSGI)) {
		glxext.has_GLX_SGI_video_sync = false;
	}
	if (!lookup(glXSwapIntervalEXT)) {
		glxext.has_GLX_EXT_swap_control = false;
	}
	if (!lookup(glXSwapIntervalMESA) || !lookup(glXGetSwapIntervalMESA)) {
		glxext.has_GLX_MESA_swap_control = false;
	}
	if (!lookup(glXSwapIntervalSGI)) {
		glxext.has_GLX_SGI_swap_control = false;
	}
	if (!lookup(glXWaitForMscOML) || !lookup(glXGetSyncValuesOML)) {
		glxext.has_GLX_OML_sync_control = false;
	}
	if (!lookup(glXBindTexImageEXT) || !lookup(glXReleaseTexImageEXT)) {
		glxext.has_GLX_EXT_texture_from_pixmap = false;
	}
	if (!lookup(glXCreateContextAttribsARB)) {
		glxext.has_GLX_ARB_create_context = false;
	}
#ifdef GLX_MESA_query_renderer
	if (!lookup(glXQueryCurrentRendererIntegerMESA)) {
		glxext.has_GLX_MESA_query_renderer = false;
	}
#endif
#undef lookup
}
