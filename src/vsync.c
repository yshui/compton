// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

/// Function pointers to init VSync modes.

#include "common.h"
#include "log.h"

#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#include "opengl.h"
#endif

#ifdef CONFIG_VSYNC_DRM
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "config.h"
#include "vsync.h"

#ifdef CONFIG_VSYNC_DRM
/**
 * Wait for next VSync, DRM method.
 *
 * Stolen from:
 * https://github.com/MythTV/mythtv/blob/master/mythtv/libs/libmythtv/vsync.cpp
 */
static int vsync_drm_wait(session_t *ps) {
	int ret = -1;
	drm_wait_vblank_t vbl;

	vbl.request.type = _DRM_VBLANK_RELATIVE, vbl.request.sequence = 1;

	do {
		ret = ioctl(ps->drm_fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
		vbl.request.type &= ~(uint)_DRM_VBLANK_RELATIVE;
	} while (ret && errno == EINTR);

	if (ret)
		log_error("VBlank ioctl did not work, unimplemented in this drmver?");

	return ret;
}

/**
 * Initialize DRM VSync.
 *
 * @return true for success, false otherwise
 */
static bool vsync_drm_init(session_t *ps) {
	// Should we always open card0?
	if (ps->drm_fd < 0 && (ps->drm_fd = open("/dev/dri/card0", O_RDWR)) < 0) {
		log_error("Failed to open device.");
		return false;
	}

	if (vsync_drm_wait(ps))
		return false;

	return true;
}
#endif

#ifdef CONFIG_OPENGL
/**
 * Initialize OpenGL VSync.
 *
 * Stolen from:
 * http://git.tuxfamily.org/?p=ccm/cairocompmgr.git;a=commitdiff;h=efa4ceb97da501e8630ca7f12c99b1dce853c73e
 * Possible original source: http://www.inb.uni-luebeck.de/~boehme/xvideo_sync.html
 *
 * @return true for success, false otherwise
 */
static bool vsync_opengl_init(session_t *ps) {
	if (!ensure_glx_context(ps))
		return false;

	return glxext.has_GLX_SGI_video_sync;
}

static bool vsync_opengl_oml_init(session_t *ps) {
	if (!ensure_glx_context(ps))
		return false;

	return glxext.has_GLX_OML_sync_control;
}

static inline bool vsync_opengl_swc_swap_interval(session_t *ps, int interval) {
	if (glxext.has_GLX_EXT_swap_control) {
		GLXDrawable d = glXGetCurrentDrawable();
		if (d == None) {
			// We don't have a context??
			return false;
		}
		glXSwapIntervalEXT(ps->dpy, glXGetCurrentDrawable(), glxext.has_GLX_EXT_swap_control_tear ? -interval : interval);
		return true;
	}
	else if (glxext.has_GLX_MESA_swap_control)
		return glXSwapIntervalMESA((uint)interval) == 0;
	else if (glxext.has_GLX_SGI_swap_control)
		return glXSwapIntervalSGI(interval) == 0;
	return false;
}

static bool vsync_opengl_swc_init(session_t *ps) {
	if (!bkend_use_glx(ps)) {
		log_error("OpenGL swap control requires the GLX backend.");
		return false;
	}

	if (!vsync_opengl_swc_swap_interval(ps, 1)) {
		log_error("Failed to load a swap control extension.");
		return false;
	}

	return true;
}

/**
 * Wait for next VSync, OpenGL method.
 */
static int vsync_opengl_wait(session_t *ps attr_unused) {
	unsigned vblank_count = 0;

	glXGetVideoSyncSGI(&vblank_count);
	glXWaitVideoSyncSGI(2, (vblank_count + 1) % 2, &vblank_count);
	return 0;
}

/**
 * Wait for next VSync, OpenGL OML method.
 *
 * https://mail.gnome.org/archives/clutter-list/2012-November/msg00031.html
 */
static int vsync_opengl_oml_wait(session_t *ps) {
	int64_t ust = 0, msc = 0, sbc = 0;

	glXGetSyncValuesOML(ps->dpy, ps->reg_win, &ust, &msc, &sbc);
	glXWaitForMscOML(ps->dpy, ps->reg_win, 0, 2, (msc + 1) % 2, &ust, &msc, &sbc);
	return 0;
}
#endif

/**
 * Initialize current VSync method.
 */
bool vsync_init(session_t *ps) {
#ifdef CONFIG_OPENGL
	if (bkend_use_glx(ps)) {
		// Mesa turns on swap control by default, undo that
		vsync_opengl_swc_swap_interval(ps, 0);
	}
#endif
#ifdef CONFIG_VSYNC_DRM
	log_warn("The DRM vsync method is deprecated, please don't enable it.");
#endif

	if (!ps->o.vsync) {
		return true;
	}

#ifdef CONFIG_OPENGL
	if (bkend_use_glx(ps)) {
		if (!vsync_opengl_swc_init(ps)) {
			return false;
		}
		ps->vsync_wait = NULL;        // glXSwapBuffers will automatically wait
		                              // for vsync, we don't need to do anything.
		return true;
	}
#endif

	// Oh no, we are not using glx backend.
	// Throwing things at wall.
#ifdef CONFIG_OPENGL
	if (vsync_opengl_oml_init(ps)) {
		log_info("Using the opengl-oml vsync method");
		ps->vsync_wait = vsync_opengl_oml_wait;
		return true;
	}

	if (vsync_opengl_init(ps)) {
		log_info("Using the opengl vsync method");
		ps->vsync_wait = vsync_opengl_wait;
		return true;
	}
#endif

#ifdef CONFIG_VSYNC_DRM
	if (vsync_drm_init(ps)) {
		log_info("Using the drm vsync method");
		ps->vsync_wait = vsync_drm_wait;
		return true;
	}
#endif

	log_error("No supported vsync method found for this backend");
	return false;
}
