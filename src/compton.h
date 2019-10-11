// SPDX-License-Identifier: MIT
// Copyright (c)

// Throw everything in here.
// !!! DON'T !!!

// === Includes ===

#include <locale.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xcb/xproto.h>

#include <X11/Xutil.h>
#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"        // XXX clean up
#include "region.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "x.h"

enum root_flags { ROOT_FLAGS_SCREEN_CHANGE = 1 };

// == Functions ==
// TODO move static inline functions that are only used in compton.c, into
//      compton.c

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

void add_damage(session_t *ps, const region_t *damage);

uint32_t determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode);

xcb_window_t find_client_win(session_t *ps, xcb_window_t w);

/// Handle configure event of a root window
void configure_root(session_t *ps, int width, int height);

void circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce);

void update_refresh_rate(session_t *ps);

void root_damaged(session_t *ps);

void cxinerama_upd_scrs(session_t *ps);

void queue_redraw(session_t *ps);

void discard_ignore(session_t *ps, unsigned long sequence);

void set_root_flags(session_t *ps, uint64_t flags);

void quit_compton(session_t *ps);

xcb_window_t session_get_target_window(session_t *);

/**
 * Set a <code>switch_t</code> array of all unset wintypes to true.
 */
static inline void wintype_arr_enable_unset(switch_t arr[]) {
	wintype_t i;

	for (i = 0; i < NUM_WINTYPES; ++i)
		if (UNSET == arr[i])
			arr[i] = ON;
}

/**
 * Check if a window ID exists in an array of window IDs.
 *
 * @param arr the array of window IDs
 * @param count amount of elements in the array
 * @param wid window ID to search for
 */
static inline bool array_wid_exists(const xcb_window_t *arr, int count, xcb_window_t wid) {
	while (count--) {
		if (arr[count] == wid) {
			return true;
		}
	}

	return false;
}

/**
 * Destroy a condition list.
 */
static inline void free_wincondlst(c2_lptr_t **pcondlst) {
	while ((*pcondlst = c2_free_lptr(*pcondlst)))
		continue;
}

/**
 * Create a XTextProperty of a single string.
 */
static inline XTextProperty *make_text_prop(session_t *ps, char *str) {
	XTextProperty *pprop = ccalloc(1, XTextProperty);

	if (XmbTextListToTextProperty(ps->dpy, &str, 1, XStringStyle, pprop)) {
		XFree(pprop->value);
		free(pprop);
		pprop = NULL;
	}

	return pprop;
}

/**
 * Set a single-string text property on a window.
 */
static inline bool
wid_set_text_prop(session_t *ps, xcb_window_t wid, xcb_atom_t prop_atom, char *str) {
	XTextProperty *pprop = make_text_prop(ps, str);
	if (!pprop) {
		log_error("Failed to make text property: %s.", str);
		return false;
	}

	XSetTextProperty(ps->dpy, wid, pprop, prop_atom);
	XFree(pprop->value);
	XFree(pprop);

	return true;
}

/**
 * Dump an drawable's info.
 */
static inline void dump_drawable(session_t *ps, xcb_drawable_t drawable) {
	auto r = xcb_get_geometry_reply(ps->c, xcb_get_geometry(ps->c, drawable), NULL);
	if (!r) {
		log_trace("Drawable %#010x: Failed", drawable);
		return;
	}
	log_trace("Drawable %#010x: x = %u, y = %u, wid = %u, hei = %d, b = %u, d = %u",
	          drawable, r->x, r->y, r->width, r->height, r->border_width, r->depth);
	free(r);
}
