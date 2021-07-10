// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <stdbool.h>
// Older version of glx.h defines function prototypes for these extensions...
// Rename them to avoid conflicts
#define glXSwapIntervalMESA glXSwapIntervalMESA_
#define glXBindTexImageEXT glXBindTexImageEXT_
#define glXReleaseTexImageEXT glXReleaseTexImageEXT
#include <GL/glx.h>
#undef glXSwapIntervalMESA
#undef glXBindTexImageEXT
#undef glXReleaseTexImageEXT
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <xcb/render.h>

#include "log.h"
#include "compiler.h"
#include "utils.h"
#include "x.h"

struct glx_fbconfig_info {
	GLXFBConfig cfg;
	int texture_tgts;
	int texture_fmt;
	int y_inverted;
};

/// The search criteria for glx_find_fbconfig
struct glx_fbconfig_criteria {
	/// Bit width of the red component
	int red_size;
	/// Bit width of the green component
	int green_size;
	/// Bit width of the blue component
	int blue_size;
	/// Bit width of the alpha component
	int alpha_size;
	/// The depth of X visual
	int visual_depth;
};

struct glx_fbconfig_info *glx_find_fbconfig(Display *, int screen, struct xvisual_info);


struct glxext_info {
	bool initialized;
	bool has_GLX_SGI_video_sync;
	bool has_GLX_SGI_swap_control;
	bool has_GLX_OML_sync_control;
	bool has_GLX_MESA_swap_control;
	bool has_GLX_EXT_swap_control;
	bool has_GLX_EXT_swap_control_tear;
	bool has_GLX_EXT_texture_from_pixmap;
	bool has_GLX_ARB_create_context;
	bool has_GLX_EXT_buffer_age;
	bool has_GLX_MESA_query_renderer;
};

extern struct glxext_info glxext;

extern PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI;
extern PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI;
extern PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML;
extern PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML;
extern PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
extern PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
extern PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
extern PFNGLXGETSWAPINTERVALMESAPROC glXGetSwapIntervalMESA;
extern PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT;
extern PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;
extern PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB;

#ifdef GLX_MESA_query_renderer
extern PFNGLXQUERYCURRENTRENDERERINTEGERMESAPROC glXQueryCurrentRendererIntegerMESA;
#endif

void glxext_init(Display *, int screen);
