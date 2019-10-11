// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>
 *
 * See LICENSE-mit for more information.
 *
 */

#pragma once

// === Options ===

// Debug options, enable them using -D in CFLAGS
// #define DEBUG_REPAINT    1
// #define DEBUG_EVENTS     1
// #define DEBUG_RESTACK    1
// #define DEBUG_WINMATCH   1
// #define DEBUG_C2         1
// #define DEBUG_GLX_DEBUG_CONTEXT        1

#define MAX_ALPHA (255)

// === Includes ===

// For some special functions
#include <assert.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <ev.h>
#include <pixman.h>
#include <xcb/xproto.h>
#include <xcb/render.h>
#include <xcb/sync.h>

#include "uthash_extra.h"

// X resource checker
#ifdef DEBUG_XRC
#include "xrescheck.h"
#endif

// FIXME This list of includes should get shorter
#include "backend/backend.h"
#include "backend/driver.h"
#include "compiler.h"
#include "config.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "list.h"
#include "win_defs.h"
#include "x.h"

// === Constants ===0

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

/// @brief Maximum OpenGL FBConfig depth.
#define OPENGL_MAX_DEPTH 32

/// @brief Maximum OpenGL buffer age.
#define CGLX_MAX_BUFFER_AGE 5

// Window flags

// === Types ===
typedef struct glx_fbconfig glx_fbconfig_t;
struct glx_session;
struct atom;
struct conv;

typedef struct _ignore {
	struct _ignore *next;
	unsigned long sequence;
} ignore_t;

/// Linked list type of atoms.
typedef struct _latom {
	xcb_atom_t atom;
	struct _latom *next;
} latom_t;

/// Structure containing all necessary data for a compton session.
typedef struct session {
	// === Event handlers ===
	/// ev_io for X connection
	ev_io xiow;
	/// Timeout for delayed unredirection.
	ev_timer unredir_timer;
	/// Timer for fading
	ev_timer fade_timer;
	/// Timer for delayed drawing, right now only used by
	/// swopti
	ev_timer delayed_draw_timer;
	/// Use an ev_idle callback for drawing
	/// So we only start drawing when events are processed
	ev_idle draw_idle;
	/// Called everytime we have timeouts or new data on socket,
	/// so we can be sure if xcb read from X socket at anytime during event
	/// handling, we will not left any event unhandled in the queue
	ev_prepare event_check;
	/// Signal handler for SIGUSR1
	ev_signal usr1_signal;
	/// Signal handler for SIGINT
	ev_signal int_signal;
	/// backend data
	backend_t *backend_data;
	/// backend blur context
	void *backend_blur_context;
	/// graphic drivers used
	enum driver drivers;
	/// libev mainloop
	struct ev_loop *loop;

	// === Display related ===
	/// Whether the X server is grabbed by us
	bool server_grabbed;
	/// Display in use.
	Display *dpy;
	/// Previous handler of X errors
	XErrorHandler previous_xerror_handler;
	/// Default screen.
	int scr;
	/// XCB connection.
	xcb_connection_t *c;
	/// Default visual.
	xcb_visualid_t vis;
	/// Default depth.
	int depth;
	/// Root window.
	xcb_window_t root;
	/// Height of root window.
	int root_height;
	/// Width of root window.
	int root_width;
	// Damage of root window.
	// Damage root_damage;
	/// X Composite overlay window. Used if <code>--paint-on-overlay</code>.
	xcb_window_t overlay;
	/// The target window for debug mode
	xcb_window_t debug_window;
	/// Whether the root tile is filled by compton.
	bool root_tile_fill;
	/// The backend data the root pixmap bound to
	void *root_image;
	/// A region of the size of the screen.
	region_t screen_reg;
	/// Window ID of the window we register as a symbol.
	xcb_window_t reg_win;
	/// Sync fence to sync draw operations
	xcb_sync_fence_t sync_fence;
	/// Whether we are rendering the first frame after screen is redirected
	bool first_frame;

	// === Operation related ===
	/// Flags related to the root window
	uint64_t root_flags;
	/// Program options.
	options_t o;
	/// Whether we have hit unredirection timeout.
	bool tmout_unredir_hit;
	/// Whether we need to redraw the screen
	bool redraw_needed;
	/// The region needs to painted on next paint.
	region_t *damage;
	/// The region damaged on the last paint.
	region_t *damage_ring;
	/// Number of damage regions we track
	int ndamage;
	/// Whether all windows are currently redirected.
	bool redirected;
	/// Pre-generated alpha pictures.
	xcb_render_picture_t *alpha_picts;
	/// Time of last fading. In milliseconds.
	long fade_time;
	/// Head pointer of the error ignore linked list.
	ignore_t *ignore_head;
	/// Pointer to the <code>next</code> member of tail element of the error
	/// ignore linked list.
	ignore_t **ignore_tail;
	// Cached blur convolution kernels.
	struct x_convolution_kernel **blur_kerns_cache;
	/// If compton should quit
	bool quit:1;
	/// Whether there are pending updates, like window creation, etc.
	/// TODO use separate flags for dfferent kinds of updates so we don't
	/// waste our time.
	bool pending_updates:1;

	// === Expose event related ===
	/// Pointer to an array of <code>XRectangle</code>-s of exposed region.
	/// XXX why do we need this array?
	rect_t *expose_rects;
	/// Number of <code>XRectangle</code>-s in <code>expose_rects</code>.
	int size_expose;
	/// Index of the next free slot in <code>expose_rects</code>.
	int n_expose;

	// === Window related ===
	/// A hash table of all windows.
	struct win *windows;
	/// Windows in their stacking order
	struct list_node window_stack;
	/// Pointer to <code>win</code> of current active window. Used by
	/// EWMH <code>_NET_ACTIVE_WINDOW</code> focus detection. In theory,
	/// it's more reliable to store the window ID directly here, just in
	/// case the WM does something extraordinary, but caching the pointer
	/// means another layer of complexity.
	struct managed_win *active_win;
	/// Window ID of leader window of currently active window. Used for
	/// subsidiary window detection.
	xcb_window_t active_leader;

	// === Shadow/dimming related ===
	/// 1x1 black Picture.
	xcb_render_picture_t black_picture;
	/// 1x1 Picture of the shadow color.
	xcb_render_picture_t cshadow_picture;
	/// 1x1 white Picture.
	xcb_render_picture_t white_picture;
	/// Gaussian map of shadow.
	struct conv *gaussian_map;
	// for shadow precomputation
	/// A region in which shadow is not painted on.
	region_t shadow_exclude_reg;

	// === Software-optimization-related ===
	/// Currently used refresh rate.
	int refresh_rate;
	/// Interval between refresh in nanoseconds.
	long refresh_intv;
	/// Nanosecond offset of the first painting.
	long paint_tm_offset;

	// === X extension related ===
	/// Event base number for X Fixes extension.
	int xfixes_event;
	/// Error base number for X Fixes extension.
	int xfixes_error;
	/// Event base number for X Damage extension.
	int damage_event;
	/// Error base number for X Damage extension.
	int damage_error;
	/// Event base number for X Render extension.
	int render_event;
	/// Error base number for X Render extension.
	int render_error;
	/// Event base number for X Composite extension.
	int composite_event;
	/// Error base number for X Composite extension.
	int composite_error;
	/// Major opcode for X Composite extension.
	int composite_opcode;
	/// Whether X Shape extension exists.
	bool shape_exists;
	/// Event base number for X Shape extension.
	int shape_event;
	/// Error base number for X Shape extension.
	int shape_error;
	/// Whether X RandR extension exists.
	bool randr_exists;
	/// Event base number for X RandR extension.
	int randr_event;
	/// Error base number for X RandR extension.
	int randr_error;
	/// Whether X Present extension exists.
	bool present_exists;
	/// Whether X Xinerama extension exists.
	bool xinerama_exists;
	/// Xinerama screen regions.
	region_t *xinerama_scr_regs;
	/// Number of Xinerama screens.
	int xinerama_nscrs;
	/// Whether X Sync extension exists.
	bool xsync_exists;
	/// Event base number for X Sync extension.
	int xsync_event;
	/// Error base number for X Sync extension.
	int xsync_error;
	/// Whether X Render convolution filter exists.
	bool xrfilter_convolution_exists;

	// === Atoms ===
	struct atom *atoms;
	/// Array of atoms of all possible window types.
	xcb_atom_t atoms_wintypes[NUM_WINTYPES];
	/// Linked list of additional atoms to track.
	latom_t *track_atom_lst;

#ifdef CONFIG_DBUS
	// === DBus related ===
	void *dbus_data;
#endif
} session_t;

/// Enumeration for window event hints.
typedef enum { WIN_EVMODE_UNKNOWN, WIN_EVMODE_FRAME, WIN_EVMODE_CLIENT } win_evmode_t;

extern const char *const WINTYPES[NUM_WINTYPES];
extern session_t *ps_g;

void ev_xcb_error(session_t *ps, xcb_generic_error_t *err);

// === Functions ===

/**
 * Subtracting two struct timespec values.
 *
 * Taken from glibc manual.
 *
 * Subtract the `struct timespec' values X and Y,
 * storing the result in RESULT.
 * Return 1 if the difference is negative, otherwise 0.
 */
static inline int
timespec_subtract(struct timespec *result, struct timespec *x, struct timespec *y) {
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_nsec < y->tv_nsec) {
		long nsec = (y->tv_nsec - x->tv_nsec) / NS_PER_SEC + 1;
		y->tv_nsec -= NS_PER_SEC * nsec;
		y->tv_sec += nsec;
	}

	if (x->tv_nsec - y->tv_nsec > NS_PER_SEC) {
		long nsec = (x->tv_nsec - y->tv_nsec) / NS_PER_SEC;
		y->tv_nsec += NS_PER_SEC * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait.
	   tv_nsec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_nsec = x->tv_nsec - y->tv_nsec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

/**
 * Get current time in struct timeval.
 */
static inline struct timeval get_time_timeval(void) {
	struct timeval tv = {0, 0};

	gettimeofday(&tv, NULL);

	// Return a time of all 0 if the call fails
	return tv;
}

/**
 * Get current time in struct timespec.
 *
 * Note its starting time is unspecified.
 */
static inline struct timespec get_time_timespec(void) {
	struct timespec tm = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &tm);

	// Return a time of all 0 if the call fails
	return tm;
}

/**
 * Return the painting target window.
 */
static inline xcb_window_t get_tgt_window(session_t *ps) {
	return ps->overlay != XCB_NONE ? ps->overlay : ps->root;
}

/**
 * Check if current backend uses GLX.
 */
static inline bool bkend_use_glx(session_t *ps) {
	return BKEND_GLX == ps->o.backend || BKEND_XR_GLX_HYBRID == ps->o.backend;
}

static void set_ignore(session_t *ps, unsigned long sequence) {
	if (ps->o.show_all_xerrors)
		return;

	auto i = cmalloc(ignore_t);
	if (!i)
		return;

	i->sequence = sequence;
	i->next = 0;
	*ps->ignore_tail = i;
	ps->ignore_tail = &i->next;
}

/**
 * Ignore X errors caused by given X request.
 */
static inline void set_ignore_cookie(session_t *ps, xcb_void_cookie_t cookie) {
	set_ignore(ps, cookie.sequence);
}

/**
 * Determine if a window has a specific property.
 *
 * @param ps current session
 * @param w window to check
 * @param atom atom of property to check
 * @return true if it has the attribute, false otherwise
 */
static inline bool wid_has_prop(const session_t *ps, xcb_window_t w, xcb_atom_t atom) {
	auto r = xcb_get_property_reply(
	    ps->c, xcb_get_property(ps->c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 0), NULL);
	if (!r) {
		return false;
	}

	auto rtype = r->type;
	free(r);

	if (rtype != XCB_NONE) {
		return true;
	}
	return false;
}

void force_repaint(session_t *ps);

/** @name DBus handling
 */
///@{
#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{
void opts_set_no_fading_openclose(session_t *ps, bool newval);
//!@}
#endif

/**
 * Set a <code>bool</code> array of all wintypes to true.
 */
static inline void wintype_arr_enable(bool arr[]) {
	wintype_t i;

	for (i = 0; i < NUM_WINTYPES; ++i) {
		arr[i] = true;
	}
}
