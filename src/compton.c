// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xfixes.h>

#include <ev.h>
#include <test.h>

#include "common.h"
#include "compiler.h"
#include "compton.h"
#include "err.h"
#include "kernel.h"
#ifdef CONFIG_OPENGL
#include "opengl.h"
#endif
#include "backend/backend.h"
#include "c2.h"
#include "config.h"
#include "diagnostic.h"
#include "log.h"
#include "region.h"
#include "render.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "x.h"
#ifdef CONFIG_DBUS
#include "dbus.h"
#endif
#include "event.h"
#include "options.h"
#include "uthash_extra.h"

/// Get session_t pointer from a pointer to a member of session_t
#define session_ptr(ptr, member)                                                         \
	({                                                                               \
		const __typeof__(((session_t *)0)->member) *__mptr = (ptr);              \
		(session_t *)((char *)__mptr - offsetof(session_t, member));             \
	})

static bool must_use redir_start(session_t *ps);

static void redir_stop(session_t *ps);

// === Global constants ===

/// Name strings for window types.
const char *const WINTYPES[NUM_WINTYPES] = {
    "unknown",    "desktop", "dock",   "toolbar", "menu",
    "utility",    "splash",  "dialog", "normal",  "dropdown_menu",
    "popup_menu", "tooltip", "notify", "combo",   "dnd",
};

/// Names of backends.
const char *const BACKEND_STRS[NUM_BKEND + 1] = {"xrender",              // BKEND_XRENDER
                                                 "glx",                  // BKEND_GLX
                                                 "xr_glx_hybrid",        // BKEND_XR_GLX_HYBRID
                                                 NULL};

// === Global variables ===

/// Pointer to current session, as a global variable. Only used by
/// xerror(), which could not have a pointer to current session passed in.
/// XXX Limit what xerror can access by not having this pointer
session_t *ps_g = NULL;

void set_root_flags(session_t *ps, uint64_t flags) {
	ps->root_flags |= flags;
}

/**
 * Free Xinerama screen info.
 *
 * XXX consider moving to x.c
 */
static inline void free_xinerama_info(session_t *ps) {
	if (ps->xinerama_scr_regs) {
		for (int i = 0; i < ps->xinerama_nscrs; ++i)
			pixman_region32_fini(&ps->xinerama_scr_regs[i]);
		free(ps->xinerama_scr_regs);
	}
	free(ps->xinerama_scrs);
	ps->xinerama_scrs = NULL;
	ps->xinerama_nscrs = 0;
}

/**
 * Get current system clock in milliseconds.
 */
static inline int64_t get_time_ms(void) {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (int64_t)tp.tv_sec * 1000 + (int64_t)tp.tv_nsec / 1000000;
}

// XXX Move to x.c
void cxinerama_upd_scrs(session_t *ps) {
	// XXX Consider deprecating Xinerama, switch to RandR when necessary
	free_xinerama_info(ps);

	if (!ps->o.xinerama_shadow_crop || !ps->xinerama_exists)
		return;

	xcb_xinerama_is_active_reply_t *active =
	    xcb_xinerama_is_active_reply(ps->c, xcb_xinerama_is_active(ps->c), NULL);
	if (!active || !active->state) {
		free(active);
		return;
	}
	free(active);

	ps->xinerama_scrs =
	    xcb_xinerama_query_screens_reply(ps->c, xcb_xinerama_query_screens(ps->c), NULL);
	if (!ps->xinerama_scrs)
		return;

	xcb_xinerama_screen_info_t *scrs =
	    xcb_xinerama_query_screens_screen_info(ps->xinerama_scrs);
	ps->xinerama_nscrs = xcb_xinerama_query_screens_screen_info_length(ps->xinerama_scrs);

	ps->xinerama_scr_regs = ccalloc(ps->xinerama_nscrs, region_t);
	for (int i = 0; i < ps->xinerama_nscrs; ++i) {
		const xcb_xinerama_screen_info_t *const s = &scrs[i];
		pixman_region32_init_rect(&ps->xinerama_scr_regs[i], s->x_org, s->y_org,
		                          s->width, s->height);
	}
}

/**
 * Find matched window.
 *
 * XXX move to win.c
 */
static inline win *find_win_all(session_t *ps, const xcb_window_t wid) {
	if (!wid || PointerRoot == wid || wid == ps->root || wid == ps->overlay)
		return NULL;

	win *w = find_win(ps, wid);
	if (!w)
		w = find_toplevel(ps, wid);
	if (!w)
		w = find_toplevel2(ps, wid);
	return w;
}

void queue_redraw(session_t *ps) {
	// If --benchmark is used, redraw is always queued
	if (!ps->redraw_needed && !ps->o.benchmark)
		ev_idle_start(ps->loop, &ps->draw_idle);
	ps->redraw_needed = true;
}

/**
 * Get a region of the screen size.
 */
static inline void get_screen_region(session_t *ps, region_t *res) {
	pixman_box32_t b = {.x1 = 0, .y1 = 0, .x2 = ps->root_width, .y2 = ps->root_height};
	pixman_region32_fini(res);
	pixman_region32_init_rects(res, &b, 1);
}

void add_damage(session_t *ps, const region_t *damage) {
	// Ignore damage when screen isn't redirected
	if (!ps->redirected)
		return;

	if (!damage)
		return;
	pixman_region32_union(ps->damage, ps->damage, (region_t *)damage);
}

// === Fading ===

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static double fade_timeout(session_t *ps) {
	auto now = get_time_ms();
	if (ps->o.fade_delta + ps->fade_time < now)
		return 0;

	auto diff = ps->o.fade_delta + ps->fade_time - now;

	diff = clamp(diff, 0, ps->o.fade_delta * 2);

	return (double)diff / 1000.0;
}

/**
 * Run fading on a window.
 *
 * @param steps steps of fading
 * @return whether we are still in fading mode
 */
static bool run_fade(session_t *ps, win **_w, long steps) {
	win *w = *_w;
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		// We are not fading
		assert(w->opacity_tgt == w->opacity);
		return false;
	}

	if (!win_should_fade(ps, w)) {
		log_debug("Window %#010x %s doesn't need fading", w->id, w->name);
		w->opacity = w->opacity_tgt;
	}
	if (w->opacity == w->opacity_tgt) {
		// We have reached target opacity.
		// We don't call win_check_fade_finished here because that could destroy
		// the window, but we still need the damage info from this window
		log_debug("Fading finished for window %#010x %s", w->id, w->name);
		return false;
	}

	if (steps) {
		if (w->opacity < w->opacity_tgt) {
			w->opacity = clamp(w->opacity + ps->o.fade_in_step * (double)steps,
			                   0.0, w->opacity_tgt);
		} else {
			w->opacity = clamp(w->opacity - ps->o.fade_out_step * (double)steps,
			                   w->opacity_tgt, 1);
		}
	}

	// Note even if opacity == opacity_tgt here, we still want to run preprocess one
	// last time to finish state transition. So return true in that case too.
	return true;
}

// === Error handling ===

void discard_ignore(session_t *ps, unsigned long sequence) {
	while (ps->ignore_head) {
		if (sequence > ps->ignore_head->sequence) {
			ignore_t *next = ps->ignore_head->next;
			free(ps->ignore_head);
			ps->ignore_head = next;
			if (!ps->ignore_head) {
				ps->ignore_tail = &ps->ignore_head;
			}
		} else {
			break;
		}
	}
}

static int should_ignore(session_t *ps, unsigned long sequence) {
	discard_ignore(ps, sequence);
	return ps->ignore_head && ps->ignore_head->sequence == sequence;
}

// === Windows ===

/**
 * Determine the event mask for a window.
 */
uint32_t determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode) {
	uint32_t evmask = 0;
	win *w = NULL;

	// Check if it's a mapped frame window
	if (WIN_EVMODE_FRAME == mode ||
	    ((w = find_win(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
		evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
		if (ps->o.track_focus && !ps->o.use_ewmh_active_win)
			evmask |= XCB_EVENT_MASK_FOCUS_CHANGE;
	}

	// Check if it's a mapped client window
	if (WIN_EVMODE_CLIENT == mode ||
	    ((w = find_toplevel(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
		if (ps->o.frame_opacity > 0 || ps->o.track_wdata || ps->track_atom_lst ||
		    ps->o.detect_client_opacity)
			evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	}

	return evmask;
}

/**
 * Find out the WM frame of a client window by querying X.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
win *find_toplevel2(session_t *ps, xcb_window_t wid) {
	// TODO this should probably be an "update tree", then find_toplevel.
	//      current approach is a bit more "racy"
	win *w = NULL;

	// We traverse through its ancestors to find out the frame
	while (wid && wid != ps->root && !(w = find_win(ps, wid))) {
		xcb_query_tree_reply_t *reply;

		// xcb_query_tree probably fails if you run compton when X is somehow
		// initializing (like add it in .xinitrc). In this case
		// just leave it alone.
		reply = xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, wid), NULL);
		if (reply == NULL) {
			break;
		}

		wid = reply->parent;

		free(reply);
	}

	return w;
}

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to true.
 *
 * @param ps current session
 * @return struct _win of currently focused window, NULL if not found
 */
win *recheck_focus(session_t *ps) {
	// Use EWMH _NET_ACTIVE_WINDOW if enabled
	if (ps->o.use_ewmh_active_win) {
		update_ewmh_active_win(ps);
		return ps->active_win;
	}

	// Determine the currently focused window so we can apply appropriate
	// opacity on it
	xcb_window_t wid = XCB_NONE;
	xcb_get_input_focus_reply_t *reply =
	    xcb_get_input_focus_reply(ps->c, xcb_get_input_focus(ps->c), NULL);

	if (reply) {
		wid = reply->focus;
		free(reply);
	}

	win *w = find_win_all(ps, wid);

	log_trace("%#010" PRIx32 " (%#010lx \"%s\") focused.", wid,
	          (w ? w->id : XCB_NONE), (w ? w->name : NULL));

	// And we set the focus state here
	if (w) {
		win_set_focused(ps, w, true);
		return w;
	}

	return NULL;
}

/**
 * Look for the client window of a particular window.
 */
xcb_window_t find_client_win(session_t *ps, xcb_window_t w) {
	if (wid_has_prop(ps, w, ps->atom_client)) {
		return w;
	}

	xcb_query_tree_reply_t *reply =
	    xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, w), NULL);
	if (!reply)
		return 0;

	xcb_window_t *children = xcb_query_tree_children(reply);
	int nchildren = xcb_query_tree_children_length(reply);
	int i;
	xcb_window_t ret = 0;

	for (i = 0; i < nchildren; ++i) {
		if ((ret = find_client_win(ps, children[i])))
			break;
	}

	free(reply);

	return ret;
}

static void handle_root_flags(session_t *ps) {
	if ((ps->root_flags & ROOT_FLAGS_SCREEN_CHANGE) != 0) {
		if (ps->o.xinerama_shadow_crop) {
			cxinerama_upd_scrs(ps);
		}

		if (ps->o.sw_opti && !ps->o.refresh_rate) {
			update_refresh_rate(ps);
			if (!ps->refresh_rate) {
				log_warn("Refresh rate detection failed. swopti will be "
				         "temporarily disabled");
			}
		}
		ps->root_flags &= ~(uint64_t)ROOT_FLAGS_SCREEN_CHANGE;
	}
}

static win *paint_preprocess(session_t *ps, bool *fade_running) {
	// XXX need better, more general name for `fade_running`. It really
	// means if fade is still ongoing after the current frame is rendered
	win *t = NULL;
	*fade_running = false;

	// Fading step calculation
	long steps = 0L;
	auto now = get_time_ms();
	if (ps->fade_time) {
		assert(now >= ps->fade_time);
		steps = (now - ps->fade_time) / ps->o.fade_delta;
	} else {
		// Reset fade_time if unset
		ps->fade_time = get_time_ms();
		steps = 0L;
	}
	ps->fade_time += steps * ps->o.fade_delta;

	// First, let's process fading
	WIN_STACK_ITER(ps, w) {
		const winmode_t mode_old = w->mode;
		const bool was_painted = w->to_paint;
		const double opacity_old = w->opacity;

		if (win_should_dim(ps, w) != w->dim) {
			w->dim = win_should_dim(ps, w);
			add_damage_from_win(ps, w);
		}

		// Run fading
		if (run_fade(ps, &w, steps)) {
			*fade_running = true;
		}

		// Add window to damaged area if its opacity changes
		// If was_painted == false, and to_paint is also false, we don't care
		// If was_painted == false, but to_paint is true, damage will be added in
		// the loop below
		if (was_painted && w->opacity != opacity_old) {
			add_damage_from_win(ps, w);
		}

		win_check_fade_finished(ps, &w);

		if (!w) {
			// the window might have been destroyed because fading finished
			continue;
		}

		if (win_has_frame(w)) {
			w->frame_opacity = ps->o.frame_opacity;
		} else {
			w->frame_opacity = 1.0;
		}

		// Update window mode
		w->mode = win_calc_mode(w);

		// Destroy all reg_ignore above when frame opaque state changes on
		// SOLID mode
		if (was_painted && w->mode != mode_old) {
			w->reg_ignore_valid = false;
		}
	}

	// Opacity will not change, from now on.
	rc_region_t *last_reg_ignore = rc_region_new();

	bool unredir_possible = false;
	// Track whether it's the highest window to paint
	bool is_highest = true;
	bool reg_ignore_valid = true;
	WIN_STACK_ITER(ps, w) {
		__label__ skip_window;
		bool to_paint = true;
		// w->to_paint remembers whether this window is painted last time
		const bool was_painted = w->to_paint;

		// Destroy reg_ignore if some window above us invalidated it
		if (!reg_ignore_valid) {
			rc_region_unref(&w->reg_ignore);
		}

		// log_trace("%d %d %s", w->a.map_state, w->ever_damaged, w->name);

		// Give up if it's not damaged or invisible, or it's unmapped and its
		// pixmap is gone (for example due to a ConfigureNotify), or when it's
		// excluded
		if (!w->ever_damaged || w->g.x + w->g.width < 1 ||
		    w->g.y + w->g.height < 1 || w->g.x >= ps->root_width ||
		    w->g.y >= ps->root_height || w->state == WSTATE_UNMAPPED ||
		    (double)w->opacity * MAX_ALPHA < 1 || w->paint_excluded) {
			to_paint = false;
		}

		if ((w->flags & WIN_FLAGS_IMAGE_ERROR) != 0) {
			to_paint = false;
		}
		// log_trace("%s %d %d %d", w->name, to_paint, w->opacity,
		// w->paint_excluded);

		// Add window to damaged area if its painting status changes
		// or opacity changes
		if (to_paint != was_painted) {
			w->reg_ignore_valid = false;
			add_damage_from_win(ps, w);
		}

		// to_paint will never change afterward
		if (!to_paint)
			goto skip_window;

		// Calculate shadow opacity
		w->shadow_opacity = ps->o.shadow_opacity * w->opacity * ps->o.frame_opacity;

		// Generate ignore region for painting to reduce GPU load
		if (!w->reg_ignore)
			w->reg_ignore = rc_region_ref(last_reg_ignore);

		// If the window is solid, we add the window region to the
		// ignored region
		// Otherwise last_reg_ignore shouldn't change
		if (w->mode == WMODE_SOLID && !ps->o.force_win_blend) {
			region_t *tmp = rc_region_new();
			if (w->frame_opacity == 1)
				*tmp = win_get_bounding_shape_global_by_val(w);
			else {
				win_get_region_noframe_local(w, tmp);
				pixman_region32_intersect(tmp, tmp, &w->bounding_shape);
				pixman_region32_translate(tmp, w->g.x, w->g.y);
			}

			pixman_region32_union(tmp, tmp, last_reg_ignore);
			rc_region_unref(&last_reg_ignore);
			last_reg_ignore = tmp;
		}

		// (Un)redirect screen
		// We could definitely unredirect the screen when there's no window to
		// paint, but this is typically unnecessary, may cause flickering when
		// fading is enabled, and could create inconsistency when the wallpaper
		// is not correctly set.
		if (ps->o.unredir_if_possible && is_highest) {
			if (win_is_solid(ps, w) &&
			    (w->frame_opacity == 1 || !win_has_frame(w)) &&
			    win_is_fullscreen(ps, w) && !w->unredir_if_possible_excluded)
				unredir_possible = true;
		}

		if ((w->flags & WIN_FLAGS_STALE_IMAGE) != 0 &&
		    (w->flags & WIN_FLAGS_IMAGE_ERROR) == 0) {
			// Image needs to be updated
			w->flags &= ~WIN_FLAGS_STALE_IMAGE;
			if (w->state != WSTATE_UNMAPPING && w->state != WSTATE_DESTROYING) {
				// If this window doesn't have an image available, don't
				// try to rebind it
				if (!win_try_rebind_image(ps, w)) {
					w->flags |= WIN_FLAGS_IMAGE_ERROR;
				}
			}
		}
		w->prev_trans = t;
		t = w;

		// If the screen is not redirected and the window has redir_ignore set,
		// this window should not cause the screen to become redirected
		if (!(ps->o.wintype_option[w->window_type].redir_ignore && !ps->redirected)) {
			is_highest = false;
		}

	skip_window:
		reg_ignore_valid = reg_ignore_valid && w->reg_ignore_valid;
		w->reg_ignore_valid = true;

		// Avoid setting w->to_paint if w is freed
		if (w) {
			w->to_paint = to_paint;
		}
	}

	rc_region_unref(&last_reg_ignore);

	// If possible, unredirect all windows and stop painting
	if (ps->o.redirected_force != UNSET) {
		unredir_possible = !ps->o.redirected_force;
	} else if (ps->o.unredir_if_possible && is_highest && !ps->redirected) {
		// If there's no window to paint, and the screen isn't redirected,
		// don't redirect it.
		unredir_possible = true;
	}
	if (unredir_possible) {
		if (ps->redirected) {
			if (!ps->o.unredir_if_possible_delay || ps->tmout_unredir_hit)
				redir_stop(ps);
			else if (!ev_is_active(&ps->unredir_timer)) {
				ev_timer_set(
				    &ps->unredir_timer,
				    (double)ps->o.unredir_if_possible_delay / 1000.0, 0);
				ev_timer_start(ps->loop, &ps->unredir_timer);
			}
		}
	} else {
		ev_timer_stop(ps->loop, &ps->unredir_timer);
		if (!ps->redirected) {
			if (!redir_start(ps)) {
				return NULL;
			}
		}
	}

	return t;
}

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void rebuild_screen_reg(session_t *ps) {
	get_screen_region(ps, &ps->screen_reg);
}

/**
 * Rebuild <code>shadow_exclude_reg</code>.
 */
static void rebuild_shadow_exclude_reg(session_t *ps) {
	bool ret = parse_geometry(ps, ps->o.shadow_exclude_reg_str, &ps->shadow_exclude_reg);
	if (!ret)
		exit(1);
}

static void restack_win(session_t *ps, win *w, xcb_window_t new_above) {
	xcb_window_t old_above;

	if (w->next) {
		old_above = w->next->id;
	} else {
		old_above = XCB_NONE;
	}
	log_debug("Restack %#010x (%s), old_above: %#010x, new_above: %#010x", w->id,
	          w->name, old_above, new_above);

	if (old_above != new_above) {
		w->reg_ignore_valid = false;
		rc_region_unref(&w->reg_ignore);
		if (w->next) {
			w->next->reg_ignore_valid = false;
			rc_region_unref(&w->next->reg_ignore);
		}

		win **prev = NULL, **prev_old = NULL;

		bool found = false;
		for (prev = &ps->window_stack; *prev; prev = &(*prev)->next) {
			if ((*prev)->id == new_above && (*prev)->state != WSTATE_DESTROYING) {
				found = true;
				break;
			}
		}

		if (new_above && !found) {
			log_error("(%#010x, %#010x): Failed to found new above window.",
			          w->id, new_above);
			return;
		}

		for (prev_old = &ps->window_stack; *prev_old; prev_old = &(*prev_old)->next) {
			if ((*prev_old) == w) {
				break;
			}
		}

		*prev_old = w->next;
		w->next = *prev;
		*prev = w;

		// add damage for this window
		add_damage_from_win(ps, w);

#ifdef DEBUG_RESTACK
		log_trace("Window stack modified. Current stack:");
		for (win *c = ps->list; c; c = c->next) {
			const char *desc = "";
			if (c->state == WSTATE_DESTROYING) {
				desc = "(D) ";
			}
			log_trace("%#010x \"%s\" %s", c->id, c->name, desc);
		}
#endif
	}
}

/// Free up all the images and deinit the backend
static void destroy_backend(session_t *ps) {
	WIN_STACK_ITER(ps, w) {
		// Wrapping up fading in progress
		win_skip_fading(ps, &w);

		// `w` might be freed by win_check_fade_finished
		if (!w) {
			continue;
		}
		if (ps->o.experimental_backends) {
			if (w->state == WSTATE_MAPPED) {
				win_release_image(ps->backend_data, w);
			} else {
				assert(!w->win_image);
				assert(!w->shadow_image);
			}
			if (ps->root_image) {
				ps->backend_data->ops->release_image(ps->backend_data,
				                                     ps->root_image);
				ps->root_image = NULL;
			}
		} else {
			free_paint(ps, &w->paint);
		}
	}

	if (ps->o.experimental_backends) {
		// deinit backend
		ps->backend_data->ops->deinit(ps->backend_data);
		ps->backend_data = NULL;
	}
}

/// Init the backend and bind all the window pixmap to backend images
static bool initialize_backend(session_t *ps) {
	if (ps->o.experimental_backends) {
		assert(!ps->backend_data);
		// Reinitialize win_data
		ps->backend_data = backend_list[ps->o.backend]->init(ps);
		ps->backend_data->ops = backend_list[ps->o.backend];
		if (!ps->backend_data) {
			log_fatal("Failed to initialize backend, aborting...");
			ps->quit = true;
			ev_break(ps->loop, EVBREAK_ALL);
			return false;
		}

		// window_stack shouldn't include window that's not in the hash table at
		// this point. Since there cannot be any fading windows.
		HASH_ITER2(ps->windows, w) {
			if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
				if (!win_bind_image(ps, w)) {
					w->flags |= WIN_FLAGS_IMAGE_ERROR;
				}
			}
		}
	}

	// The old backends binds pixmap lazily, nothing to do here
	return true;
}

/// Handle configure event of a root window
void configure_root(session_t *ps, int width, int height) {
	log_info("Root configuration changed, new geometry: %dx%d", width, height);
	// On root window changes
	bool has_root_change = false;
	if (ps->o.experimental_backends && ps->redirected) {
		has_root_change = ps->backend_data->ops->root_change != NULL;
		if (!has_root_change) {
			// deinit/reinit backend and free up resources if the backend
			// cannot handle root change
			destroy_backend(ps);
		}
	} else {
		free_paint(ps, &ps->tgt_buffer);
	}

	ps->root_width = width;
	ps->root_height = height;

	rebuild_screen_reg(ps);
	rebuild_shadow_exclude_reg(ps);
	for (int i = 0; i < ps->ndamage; i++) {
		pixman_region32_clear(&ps->damage_ring[i]);
	}
	ps->damage = ps->damage_ring + ps->ndamage - 1;

	// Invalidate reg_ignore from the top
	rc_region_unref(&ps->window_stack->reg_ignore);
	ps->window_stack->reg_ignore_valid = false;

#ifdef CONFIG_OPENGL
	// GLX root change callback
	if (BKEND_GLX == ps->o.backend && !ps->o.experimental_backends) {
		glx_on_root_change(ps);
	}
#endif
	if (ps->o.experimental_backends && ps->redirected) {
		if (has_root_change) {
			ps->backend_data->ops->root_change(ps->backend_data, ps);
		} else {
			if (!initialize_backend(ps)) {
				log_fatal("Failed to re-initialize backend after root "
				          "change, aborting...");
				ps->quit = true;
				// TODO only event handlers should request ev_break,
				// otherwise it's too hard to keep track of what can break
				// the event loop
				ev_break(ps->loop, EVBREAK_ALL);
				return;
			}
		}
	}
	force_repaint(ps);
	return;
}

/// Handle configure event of a regular window
void configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
	win *w = find_win(ps, ce->window);
	region_t damage;
	pixman_region32_init(&damage);

	if (!w) {
		return;
	}

	if (w->state == WSTATE_UNMAPPED || w->state == WSTATE_UNMAPPING ||
	    w->state == WSTATE_DESTROYING) {
		// Only restack the window to make sure we can handle future restack
		// notification correctly
		restack_win(ps, w, ce->above_sibling);
	} else {
		restack_win(ps, w, ce->above_sibling);
		bool factor_change = false;
		win_extents(w, &damage);

		// If window geometry change, free old extents
		if (w->g.x != ce->x || w->g.y != ce->y || w->g.width != ce->width ||
		    w->g.height != ce->height || w->g.border_width != ce->border_width)
			factor_change = true;

		w->g.x = ce->x;
		w->g.y = ce->y;

		if (w->g.width != ce->width || w->g.height != ce->height ||
		    w->g.border_width != ce->border_width) {
			log_trace("Window size changed, %dx%d -> %dx%d", w->g.width,
			          w->g.height, ce->width, ce->height);
			w->g.width = ce->width;
			w->g.height = ce->height;
			w->g.border_width = ce->border_width;
			win_on_win_size_change(ps, w);
			win_update_bounding_shape(ps, w);
		}

		region_t new_extents;
		pixman_region32_init(&new_extents);
		win_extents(w, &new_extents);
		pixman_region32_union(&damage, &damage, &new_extents);
		pixman_region32_fini(&new_extents);

		if (factor_change) {
			win_on_factor_change(ps, w);
			add_damage(ps, &damage);
			win_update_screen(ps, w);
		}
	}

	pixman_region32_fini(&damage);

	// override_redirect flag cannot be changed after window creation, as far
	// as I know, so there's no point to re-match windows here.
	w->a.override_redirect = ce->override_redirect;
}

void circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce) {
	win *w = find_win(ps, ce->window);
	xcb_window_t new_above;

	if (!w)
		return;

	if (ce->place == PlaceOnTop) {
		new_above = ps->window_stack->id;
	} else {
		new_above = XCB_NONE;
	}

	restack_win(ps, w, new_above);
}

void root_damaged(session_t *ps) {
	if (ps->root_tile_paint.pixmap) {
		free_root_tile(ps);
	}

	if (!ps->redirected) {
		return;
	}

	if (ps->o.experimental_backends) {
		if (ps->root_image) {
			ps->backend_data->ops->release_image(ps->backend_data, ps->root_image);
		}
		auto pixmap = x_get_root_back_pixmap(ps);
		if (pixmap != XCB_NONE) {
			ps->root_image = ps->backend_data->ops->bind_pixmap(
			    ps->backend_data, pixmap, x_get_visual_info(ps->c, ps->vis), false);
			ps->backend_data->ops->image_op(
			    ps->backend_data, IMAGE_OP_RESIZE_TILE, ps->root_image, NULL,
			    NULL, (int[]){ps->root_width, ps->root_height});
		}
	}

	// Mark screen damaged
	force_repaint(ps);
}

/**
 * Xlib error handler function.
 */
static int xerror(Display attr_unused *dpy, XErrorEvent *ev) {
	if (!should_ignore(ps_g, ev->serial))
		x_print_error(ev->serial, ev->request_code, ev->minor_code, ev->error_code);
	return 0;
}

/**
 * XCB error handler function.
 */
void ev_xcb_error(session_t *ps, xcb_generic_error_t *err) {
	if (!should_ignore(ps, err->sequence))
		x_print_error(err->sequence, err->major_code, err->minor_code, err->error_code);
}

/**
 * Force a full-screen repaint.
 */
void force_repaint(session_t *ps) {
	assert(pixman_region32_not_empty(&ps->screen_reg));
	queue_redraw(ps);
	add_damage(ps, &ps->screen_reg);
}

#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{

/**
 * Set w->shadow_force of a window.
 */
void win_set_shadow_force(session_t *ps, win *w, switch_t val) {
	if (val != w->shadow_force) {
		w->shadow_force = val;
		win_determine_shadow(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Set w->fade_force of a window.
 *
 * Doesn't affect fading already in progress
 */
void win_set_fade_force(session_t *ps, win *w, switch_t val) {
	w->fade_force = val;
}

/**
 * Set w->focused_force of a window.
 */
void win_set_focused_force(session_t *ps, win *w, switch_t val) {
	if (val != w->focused_force) {
		w->focused_force = val;
		win_update_focused(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Set w->invert_color_force of a window.
 */
void win_set_invert_color_force(session_t *ps, win *w, switch_t val) {
	if (val != w->invert_color_force) {
		w->invert_color_force = val;
		win_determine_invert_color(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Enable focus tracking.
 */
void opts_init_track_focus(session_t *ps) {
	// Already tracking focus
	if (ps->o.track_focus)
		return;

	ps->o.track_focus = true;

	if (!ps->o.use_ewmh_active_win) {
		// Start listening to FocusChange events
		HASH_ITER2(ps->windows, w) {
			if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
				xcb_change_window_attributes(
				    ps->c, w->id, XCB_CW_EVENT_MASK,
				    (const uint32_t[]){
				        determine_evmask(ps, w->id, WIN_EVMODE_FRAME)});
			}
		}
	}

	// Recheck focus
	recheck_focus(ps);
}

/**
 * Set no_fading_openclose option.
 *
 * Don't affect fading already in progress
 */
void opts_set_no_fading_openclose(session_t *ps, bool newval) {
	ps->o.no_fading_openclose = newval;
}

//!@}
#endif

// === Events ===

/**
 * Update current active window based on EWMH _NET_ACTIVE_WIN.
 *
 * Does not change anything if we fail to get the attribute or the window
 * returned could not be found.
 */
void update_ewmh_active_win(session_t *ps) {
	// Search for the window
	xcb_window_t wid = wid_get_prop_window(ps, ps->root, ps->atom_ewmh_active_win);
	win *w = find_win_all(ps, wid);

	// Mark the window focused. No need to unfocus the previous one.
	if (w)
		win_set_focused(ps, w, true);
}

// === Main ===

/**
 * Register a window as symbol, and initialize GLX context if wanted.
 */
static bool register_cm(session_t *ps) {
	assert(!ps->reg_win);

	ps->reg_win = xcb_generate_id(ps->c);
	auto e = xcb_request_check(
	    ps->c, xcb_create_window_checked(ps->c, XCB_COPY_FROM_PARENT, ps->reg_win, ps->root,
	                                     0, 0, 1, 1, 0, XCB_NONE, ps->vis, 0, NULL));

	if (e) {
		log_fatal("Failed to create window.");
		free(e);
		return false;
	}

	// Unredirect the window if it's redirected, just in case
	if (ps->redirected)
		xcb_composite_unredirect_window(ps->c, ps->reg_win,
		                                XCB_COMPOSITE_REDIRECT_MANUAL);

	{
		XClassHint *h = XAllocClassHint();
		if (h) {
			h->res_name = "compton";
			h->res_class = "xcompmgr";
		}
		Xutf8SetWMProperties(ps->dpy, ps->reg_win, "xcompmgr", "xcompmgr", NULL,
		                     0, NULL, NULL, h);
		cxfree(h);
	}

	// Set _NET_WM_PID
	{
		auto pid = getpid();
		xcb_change_property(ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
		                    get_atom(ps, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1,
		                    &pid);
	}

	// Set COMPTON_VERSION
	if (!wid_set_text_prop(ps, ps->reg_win, get_atom(ps, "COMPTON_VERSION"),
	                       COMPTON_VERSION)) {
		log_error("Failed to set COMPTON_VERSION.");
	}

	// Acquire X Selection _NET_WM_CM_S?
	if (!ps->o.no_x_selection) {
		unsigned len = strlen(REGISTER_PROP) + 2;
		int s = ps->scr;
		xcb_atom_t atom;

		while (s >= 10) {
			++len;
			s /= 10;
		}

		auto buf = ccalloc(len, char);
		snprintf(buf, len, REGISTER_PROP "%d", ps->scr);
		buf[len - 1] = '\0';
		atom = get_atom(ps, buf);
		free(buf);

		xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(
		    ps->c, xcb_get_selection_owner(ps->c, atom), NULL);

		if (reply && reply->owner != XCB_NONE) {
			free(reply);
			log_fatal("Another composite manager is already running");
			return false;
		}
		free(reply);
		xcb_set_selection_owner(ps->c, ps->reg_win, atom, 0);
	}

	return true;
}

/**
 * Write PID to a file.
 */
static inline bool write_pid(session_t *ps) {
	if (!ps->o.write_pid_path)
		return true;

	FILE *f = fopen(ps->o.write_pid_path, "w");
	if (unlikely(!f)) {
		log_error("Failed to write PID to \"%s\".", ps->o.write_pid_path);
		return false;
	}

	fprintf(f, "%ld\n", (long)getpid());
	fclose(f);

	return true;
}

/**
 * Fetch all required atoms and save them to a session.
 */
static void init_atoms(session_t *ps) {
	ps->atom_opacity = get_atom(ps, "_NET_WM_WINDOW_OPACITY");
	ps->atom_frame_extents = get_atom(ps, "_NET_FRAME_EXTENTS");
	ps->atom_client = get_atom(ps, "WM_STATE");
	ps->atom_name = XCB_ATOM_WM_NAME;
	ps->atom_name_ewmh = get_atom(ps, "_NET_WM_NAME");
	ps->atom_class = XCB_ATOM_WM_CLASS;
	ps->atom_role = get_atom(ps, "WM_WINDOW_ROLE");
	ps->atom_transient = XCB_ATOM_WM_TRANSIENT_FOR;
	ps->atom_client_leader = get_atom(ps, "WM_CLIENT_LEADER");
	ps->atom_ewmh_active_win = get_atom(ps, "_NET_ACTIVE_WINDOW");
	ps->atom_compton_shadow = get_atom(ps, "_COMPTON_SHADOW");

	ps->atom_win_type = get_atom(ps, "_NET_WM_WINDOW_TYPE");
	ps->atoms_wintypes[WINTYPE_UNKNOWN] = 0;
	ps->atoms_wintypes[WINTYPE_DESKTOP] = get_atom(ps, "_NET_WM_WINDOW_TYPE_DESKTOP");
	ps->atoms_wintypes[WINTYPE_DOCK] = get_atom(ps, "_NET_WM_WINDOW_TYPE_DOCK");
	ps->atoms_wintypes[WINTYPE_TOOLBAR] = get_atom(ps, "_NET_WM_WINDOW_TYPE_TOOLBAR");
	ps->atoms_wintypes[WINTYPE_MENU] = get_atom(ps, "_NET_WM_WINDOW_TYPE_MENU");
	ps->atoms_wintypes[WINTYPE_UTILITY] = get_atom(ps, "_NET_WM_WINDOW_TYPE_UTILITY");
	ps->atoms_wintypes[WINTYPE_SPLASH] = get_atom(ps, "_NET_WM_WINDOW_TYPE_SPLASH");
	ps->atoms_wintypes[WINTYPE_DIALOG] = get_atom(ps, "_NET_WM_WINDOW_TYPE_DIALOG");
	ps->atoms_wintypes[WINTYPE_NORMAL] = get_atom(ps, "_NET_WM_WINDOW_TYPE_NORMAL");
	ps->atoms_wintypes[WINTYPE_DROPDOWN_MENU] =
	    get_atom(ps, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
	ps->atoms_wintypes[WINTYPE_POPUP_MENU] =
	    get_atom(ps, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
	ps->atoms_wintypes[WINTYPE_TOOLTIP] = get_atom(ps, "_NET_WM_WINDOW_TYPE_TOOLTIP");
	ps->atoms_wintypes[WINTYPE_NOTIFY] =
	    get_atom(ps, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
	ps->atoms_wintypes[WINTYPE_COMBO] = get_atom(ps, "_NET_WM_WINDOW_TYPE_COMBO");
	ps->atoms_wintypes[WINTYPE_DND] = get_atom(ps, "_NET_WM_WINDOW_TYPE_DND");
}

/**
 * Update refresh rate info with X Randr extension.
 */
void update_refresh_rate(session_t *ps) {
	xcb_randr_get_screen_info_reply_t *randr_info = xcb_randr_get_screen_info_reply(
	    ps->c, xcb_randr_get_screen_info(ps->c, ps->root), NULL);

	if (!randr_info)
		return;
	ps->refresh_rate = randr_info->rate;
	free(randr_info);

	if (ps->refresh_rate)
		ps->refresh_intv = US_PER_SEC / ps->refresh_rate;
	else
		ps->refresh_intv = 0;
}

/**
 * Initialize refresh-rated based software optimization.
 *
 * @return true for success, false otherwise
 */
static bool swopti_init(session_t *ps) {
	log_warn("--sw-opti is going to be deprecated. If you get real benefits from "
	         "using "
	         "this option, please open an issue to let us know.");
	// Prepare refresh rate
	// Check if user provides one
	ps->refresh_rate = ps->o.refresh_rate;
	if (ps->refresh_rate)
		ps->refresh_intv = US_PER_SEC / ps->refresh_rate;

	// Auto-detect refresh rate otherwise
	if (!ps->refresh_rate && ps->randr_exists) {
		update_refresh_rate(ps);
	}

	// Turn off vsync_sw if we can't get the refresh rate
	if (!ps->refresh_rate)
		return false;

	return true;
}

/**
 * Modify a struct timeval timeout value to render at a fixed pace.
 *
 * @param ps current session
 * @param[in,out] ptv pointer to the timeout
 */
static double swopti_handle_timeout(session_t *ps) {
	if (!ps->refresh_intv)
		return 0;

	// Get the microsecond offset of the time when the we reach the timeout
	// I don't think a 32-bit long could overflow here.
	long offset = (get_time_timeval().tv_usec - ps->paint_tm_offset) % ps->refresh_intv;
	// XXX this formula dones't work if refresh rate is not a whole number
	if (offset < 0)
		offset += ps->refresh_intv;

	// If the target time is sufficiently close to a refresh time, don't add
	// an offset, to avoid certain blocking conditions.
	if (offset < SWOPTI_TOLERANCE || offset > ps->refresh_intv - SWOPTI_TOLERANCE)
		return 0;

	// Add an offset so we wait until the next refresh after timeout
	return (double)(ps->refresh_intv - offset) / 1e6;
}

/**
 * Initialize X composite overlay window.
 */
static bool init_overlay(session_t *ps) {
	xcb_composite_get_overlay_window_reply_t *reply =
	    xcb_composite_get_overlay_window_reply(
	        ps->c, xcb_composite_get_overlay_window(ps->c, ps->root), NULL);
	if (reply) {
		ps->overlay = reply->overlay_win;
		free(reply);
	} else {
		ps->overlay = XCB_NONE;
	}
	if (ps->overlay) {
		// Set window region of the overlay window, code stolen from
		// compiz-0.8.8
		xcb_generic_error_t *e;
		e = XCB_SYNCED_VOID(xcb_shape_mask, ps->c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_BOUNDING, ps->overlay, 0, 0, 0);
		if (e) {
			log_fatal("Failed to set the bounding shape of overlay, giving "
			          "up.");
			exit(1);
		}
		e = XCB_SYNCED_VOID(xcb_shape_rectangles, ps->c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
		                    ps->overlay, 0, 0, 0, NULL);
		if (e) {
			log_fatal("Failed to set the input shape of overlay, giving up.");
			exit(1);
		}

		// Listen to Expose events on the overlay
		xcb_change_window_attributes(ps->c, ps->overlay, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){XCB_EVENT_MASK_EXPOSURE});

		// Retrieve DamageNotify on root window if we are painting on an
		// overlay
		// root_damage = XDamageCreate(ps->dpy, root, XDamageReportNonEmpty);

		// Unmap overlay, firstly. But this typically does not work because
		// the window isn't created yet.
		// xcb_unmap_window(c, ps->overlay);
		// XFlush(ps->dpy);
	} else {
		log_error("Cannot get X Composite overlay window. Falling "
		          "back to painting on root window.");
	}
	log_debug("overlay = %#010x", ps->overlay);

	return ps->overlay;
}

/**
 * Redirect all windows.
 *
 * @return whether the operation succeeded or not
 */
static bool redir_start(session_t *ps) {
	assert(!ps->redirected);
	log_debug("Redirecting the screen.");

	// Map overlay window. Done firstly according to this:
	// https://bugzilla.gnome.org/show_bug.cgi?id=597014
	if (ps->overlay) {
		xcb_map_window(ps->c, ps->overlay);
	}

	xcb_composite_redirect_subwindows(ps->c, ps->root, XCB_COMPOSITE_REDIRECT_MANUAL);

	x_sync(ps->c);

	if (!initialize_backend(ps)) {
		return false;
	}

	if (ps->o.experimental_backends) {
		ps->ndamage = ps->backend_data->ops->max_buffer_age;
	} else {
		ps->ndamage = maximum_buffer_age(ps);
	}
	ps->damage_ring = ccalloc(ps->ndamage, region_t);
	ps->damage = ps->damage_ring + ps->ndamage - 1;

	for (int i = 0; i < ps->ndamage; i++) {
		pixman_region32_init(&ps->damage_ring[i]);
	}

	// Must call XSync() here
	x_sync(ps->c);

	ps->redirected = true;

	root_damaged(ps);

	// Repaint the whole screen
	force_repaint(ps);
	log_debug("Screen redirected.");
	return true;
}

/**
 * Unredirect all windows.
 */
static void redir_stop(session_t *ps) {
	assert(ps->redirected);
	log_debug("Unredirecting the screen.");

	destroy_backend(ps);

	xcb_composite_unredirect_subwindows(ps->c, ps->root, XCB_COMPOSITE_REDIRECT_MANUAL);
	// Unmap overlay window
	if (ps->overlay)
		xcb_unmap_window(ps->c, ps->overlay);

	// Free the damage ring
	for (int i = 0; i < ps->ndamage; ++i) {
		pixman_region32_fini(&ps->damage_ring[i]);
	}
	ps->ndamage = 0;
	free(ps->damage_ring);
	ps->damage_ring = ps->damage = NULL;

	// Must call XSync() here
	x_sync(ps->c);

	ps->redirected = false;
	log_debug("Screen unredirected.");
}

// Handle queued events before we go to sleep
static void handle_queued_x_events(EV_P_ ev_prepare *w, int revents) {
	session_t *ps = session_ptr(w, event_check);
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_queued_event(ps->c))) {
		ev_handle(ps, ev);
		free(ev);
	};
	// Flush because if we go into sleep when there is still
	// requests in the outgoing buffer, they will not be sent
	// for an indefinite amount of time.
	// Use XFlush here too, we might still use some Xlib functions
	// because OpenGL.
	XFlush(ps->dpy);
	xcb_flush(ps->c);
	int err = xcb_connection_has_error(ps->c);
	if (err) {
		log_fatal("X11 server connection broke (error %d)", err);
		exit(1);
	}
}

/**
 * Unredirection timeout callback.
 */
static void tmout_unredir_callback(EV_P_ ev_timer *w, int revents) {
	session_t *ps = session_ptr(w, unredir_timer);
	ps->tmout_unredir_hit = true;
	queue_redraw(ps);
}

static void fade_timer_callback(EV_P_ ev_timer *w, int revents) {
	session_t *ps = session_ptr(w, fade_timer);
	queue_redraw(ps);
}

static void _draw_callback(EV_P_ session_t *ps, int revents) {
	if (ps->o.benchmark) {
		if (ps->o.benchmark_wid) {
			win *wi = find_win(ps, ps->o.benchmark_wid);
			if (!wi) {
				log_fatal("Couldn't find specified benchmark window.");
				exit(1);
			}
			add_damage_from_win(ps, wi);
		} else {
			force_repaint(ps);
		}
	}

	// TODO xcb_grab_server
	// TODO clean up event queue

	handle_root_flags(ps);

	// TODO have a stripped down version of paint_preprocess that is used when screen
	// is not redirected. its sole purpose should be to decide whether the screen
	// should be redirected.
	bool fade_running = false;
	bool was_redirected = ps->redirected;
	win *t = paint_preprocess(ps, &fade_running);
	ps->tmout_unredir_hit = false;

	if (!was_redirected && ps->redirected) {
		// paint_preprocess redirected the screen, which might change the state of
		// some of the windows (e.g. the window image might fail to bind, and the
		// window would be put into an error state). so we rerun paint_preprocess
		// here to make sure the rendering decision we make is up-to-date
		log_debug("Re-run paint_preprocess");
		t = paint_preprocess(ps, &fade_running);
	}

	// Start/stop fade timer depends on whether window are fading
	if (!fade_running && ev_is_active(&ps->fade_timer)) {
		ev_timer_stop(ps->loop, &ps->fade_timer);
	} else if (fade_running && !ev_is_active(&ps->fade_timer)) {
		ev_timer_set(&ps->fade_timer, fade_timeout(ps), 0);
		ev_timer_start(ps->loop, &ps->fade_timer);
	}

	// If the screen is unredirected, free all_damage to stop painting
	if (ps->redirected && ps->o.stoppaint_force != ON) {
		static int paint = 0;
		if (ps->o.experimental_backends) {
			paint_all_new(ps, t, false);
		} else {
			paint_all(ps, t, false);
		}

		paint++;
		if (ps->o.benchmark && paint >= ps->o.benchmark)
			exit(0);
	}

	if (!fade_running)
		ps->fade_time = 0L;

	// TODO xcb_ungrab_server

	ps->redraw_needed = false;
}

static void draw_callback(EV_P_ ev_idle *w, int revents) {
	// This function is not used if we are using --swopti
	session_t *ps = session_ptr(w, draw_idle);

	_draw_callback(EV_A_ ps, revents);

	// Don't do painting non-stop unless we are in benchmark mode
	if (!ps->o.benchmark)
		ev_idle_stop(ps->loop, &ps->draw_idle);
}

static void delayed_draw_timer_callback(EV_P_ ev_timer *w, int revents) {
	session_t *ps = session_ptr(w, delayed_draw_timer);
	_draw_callback(EV_A_ ps, revents);

	// We might have stopped the ev_idle in delayed_draw_callback,
	// so we restart it if we are in benchmark mode
	if (ps->o.benchmark)
		ev_idle_start(EV_A_ & ps->draw_idle);
}

static void delayed_draw_callback(EV_P_ ev_idle *w, int revents) {
	// This function is only used if we are using --swopti
	session_t *ps = session_ptr(w, draw_idle);
	assert(ps->redraw_needed);
	assert(!ev_is_active(&ps->delayed_draw_timer));

	double delay = swopti_handle_timeout(ps);
	if (delay < 1e-6) {
		if (!ps->o.benchmark) {
			ev_idle_stop(ps->loop, &ps->draw_idle);
		}
		return _draw_callback(EV_A_ ps, revents);
	}

	// This is a little bit hacky. When we get to this point in code, we need
	// to update the screen , but we will only be updating after a delay, So
	// we want to stop the ev_idle, so this callback doesn't get call repeatedly
	// during the delay, we also want queue_redraw to not restart the ev_idle.
	// So we stop ev_idle and leave ps->redraw_needed to be true. (effectively,
	// ps->redraw_needed means if redraw is needed or if draw is in progress).
	//
	// We do this anyway even if we are in benchmark mode. That means we will
	// have to restart draw_idle after the draw actually happened when we are in
	// benchmark mode.
	ev_idle_stop(ps->loop, &ps->draw_idle);

	ev_timer_set(&ps->delayed_draw_timer, delay, 0);
	ev_timer_start(ps->loop, &ps->delayed_draw_timer);
}

static void x_event_callback(EV_P_ ev_io *w, int revents) {
	session_t *ps = (session_t *)w;
	xcb_generic_event_t *ev = xcb_poll_for_event(ps->c);
	if (ev) {
		ev_handle(ps, ev);
		free(ev);
	}
}

/**
 * Turn on the program reset flag.
 *
 * This will result in compton resetting itself after next paint.
 */
static void reset_enable(EV_P_ ev_signal *w, int revents) {
	session_t *ps = session_ptr(w, usr1_signal);
	log_info("compton is resetting...");
	ev_break(ps->loop, EVBREAK_ALL);
}

static void exit_enable(EV_P_ ev_signal *w, int revents) {
	session_t *ps = session_ptr(w, int_signal);
	log_info("compton is quitting...");
	ps->quit = true;
	ev_break(ps->loop, EVBREAK_ALL);
}

/**
 * Initialize a session.
 *
 * @param argc number of commandline arguments
 * @param argv commandline arguments
 * @param dpy  the X Display
 * @param config_file the path to the config file
 * @param all_xerros whether we should report all X errors
 * @param fork whether we will fork after initialization
 */
static session_t *session_init(int argc, char **argv, Display *dpy,
                               const char *config_file, bool all_xerrors, bool fork) {
	static const session_t s_def = {
	    .backend_data = NULL,
	    .dpy = NULL,
	    .scr = 0,
	    .c = NULL,
	    .vis = 0,
	    .depth = 0,
	    .root = XCB_NONE,
	    .root_height = 0,
	    .root_width = 0,
	    // .root_damage = XCB_NONE,
	    .overlay = XCB_NONE,
	    .root_tile_fill = false,
	    .root_tile_paint = PAINT_INIT,
	    .tgt_picture = XCB_NONE,
	    .tgt_buffer = PAINT_INIT,
	    .reg_win = XCB_NONE,
#ifdef CONFIG_OPENGL
	    .glx_prog_win = GLX_PROG_MAIN_INIT,
#endif
	    .o =
	        {
	            .backend = BKEND_XRENDER,
	            .glx_no_stencil = false,
	            .mark_wmwin_focused = false,
	            .mark_ovredir_focused = false,
	            .detect_rounded_corners = false,
	            .resize_damage = 0,
	            .unredir_if_possible = false,
	            .unredir_if_possible_blacklist = NULL,
	            .unredir_if_possible_delay = 0,
	            .redirected_force = UNSET,
	            .stoppaint_force = UNSET,
	            .dbus = false,
	            .benchmark = 0,
	            .benchmark_wid = XCB_NONE,
	            .logpath = NULL,

	            .refresh_rate = 0,
	            .sw_opti = false,

	            .shadow_red = 0.0,
	            .shadow_green = 0.0,
	            .shadow_blue = 0.0,
	            .shadow_radius = 18,
	            .shadow_offset_x = -15,
	            .shadow_offset_y = -15,
	            .shadow_opacity = .75,
	            .shadow_blacklist = NULL,
	            .shadow_ignore_shaped = false,
	            .respect_prop_shadow = false,
	            .xinerama_shadow_crop = false,

	            .fade_in_step = 0.028,
	            .fade_out_step = 0.03,
	            .fade_delta = 10,
	            .no_fading_openclose = false,
	            .no_fading_destroyed_argb = false,
	            .fade_blacklist = NULL,

	            .inactive_opacity = 1.0,
	            .inactive_opacity_override = false,
	            .active_opacity = 1.0,
	            .frame_opacity = 1.0,
	            .detect_client_opacity = false,

	            .blur_background = false,
	            .blur_background_frame = false,
	            .blur_background_fixed = false,
	            .blur_background_blacklist = NULL,
	            .blur_kerns = {NULL},
	            .inactive_dim = 0.0,
	            .inactive_dim_fixed = false,
	            .invert_color_list = NULL,
	            .opacity_rules = NULL,

	            .use_ewmh_active_win = false,
	            .focus_blacklist = NULL,
	            .detect_transient = false,
	            .detect_client_leader = false,

	            .track_focus = false,
	            .track_wdata = false,
	            .track_leader = false,
	        },

	    .time_start = {0, 0},
	    .redirected = false,
	    .alpha_picts = NULL,
	    .fade_time = 0L,
	    .ignore_head = NULL,
	    .ignore_tail = NULL,
	    .quit = false,

	    .expose_rects = NULL,
	    .size_expose = 0,
	    .n_expose = 0,

	    .windows = NULL,
	    .active_win = NULL,
	    .active_leader = XCB_NONE,

	    .black_picture = XCB_NONE,
	    .cshadow_picture = XCB_NONE,
	    .white_picture = XCB_NONE,
	    .gaussian_map = NULL,

	    .refresh_rate = 0,
	    .refresh_intv = 0UL,
	    .paint_tm_offset = 0L,

#ifdef CONFIG_VSYNC_DRM
	    .drm_fd = -1,
#endif

	    .xfixes_event = 0,
	    .xfixes_error = 0,
	    .damage_event = 0,
	    .damage_error = 0,
	    .render_event = 0,
	    .render_error = 0,
	    .composite_event = 0,
	    .composite_error = 0,
	    .composite_opcode = 0,
	    .shape_exists = false,
	    .shape_event = 0,
	    .shape_error = 0,
	    .randr_exists = 0,
	    .randr_event = 0,
	    .randr_error = 0,
#ifdef CONFIG_OPENGL
	    .glx_exists = false,
	    .glx_event = 0,
	    .glx_error = 0,
#endif
	    .xrfilter_convolution_exists = false,

	    .atom_opacity = XCB_NONE,
	    .atom_frame_extents = XCB_NONE,
	    .atom_client = XCB_NONE,
	    .atom_name = XCB_NONE,
	    .atom_name_ewmh = XCB_NONE,
	    .atom_class = XCB_NONE,
	    .atom_role = XCB_NONE,
	    .atom_transient = XCB_NONE,
	    .atom_ewmh_active_win = XCB_NONE,
	    .atom_compton_shadow = XCB_NONE,
	    .atom_win_type = XCB_NONE,
	    .atoms_wintypes = {0},
	    .track_atom_lst = NULL,

#ifdef CONFIG_DBUS
	    .dbus_data = NULL,
#endif
	};

	auto stderr_logger = stderr_logger_new();
	if (stderr_logger) {
		// stderr logger might fail to create if we are already
		// daemonized.
		log_add_target_tls(stderr_logger);
	}

	// Allocate a session and copy default values into it
	session_t *ps = cmalloc(session_t);
	*ps = s_def;
	ps->loop = EV_DEFAULT;
	pixman_region32_init(&ps->screen_reg);

	ps->ignore_tail = &ps->ignore_head;
	gettimeofday(&ps->time_start, NULL);

	ps->o.show_all_xerrors = all_xerrors;

	// Use the same Display across reset, primarily for resource leak checking
	ps->dpy = dpy;
	ps->c = XGetXCBConnection(ps->dpy);

	const xcb_query_extension_reply_t *ext_info;

	XSetErrorHandler(xerror);

	ps->scr = DefaultScreen(ps->dpy);

	auto screen = x_screen_of_display(ps->c, ps->scr);
	ps->vis = screen->root_visual;
	ps->depth = screen->root_depth;
	ps->root = screen->root;
	ps->root_width = screen->width_in_pixels;
	ps->root_height = screen->height_in_pixels;

	// Start listening to events on root earlier to catch all possible
	// root geometry changes
	auto e = xcb_request_check(
	    ps->c, xcb_change_window_attributes_checked(
	               ps->c, ps->root, XCB_CW_EVENT_MASK,
	               (const uint32_t[]){XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	                                  XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	                                  XCB_EVENT_MASK_PROPERTY_CHANGE}));
	if (e) {
		log_error("Failed to setup root window event mask");
		free(e);
	}

	xcb_prefetch_extension_data(ps->c, &xcb_render_id);
	xcb_prefetch_extension_data(ps->c, &xcb_composite_id);
	xcb_prefetch_extension_data(ps->c, &xcb_damage_id);
	xcb_prefetch_extension_data(ps->c, &xcb_shape_id);
	xcb_prefetch_extension_data(ps->c, &xcb_xfixes_id);
	xcb_prefetch_extension_data(ps->c, &xcb_randr_id);
	xcb_prefetch_extension_data(ps->c, &xcb_xinerama_id);
	xcb_prefetch_extension_data(ps->c, &xcb_present_id);
	xcb_prefetch_extension_data(ps->c, &xcb_sync_id);

	ext_info = xcb_get_extension_data(ps->c, &xcb_render_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No render extension");
		exit(1);
	}
	ps->render_event = ext_info->first_event;
	ps->render_error = ext_info->first_error;

	ext_info = xcb_get_extension_data(ps->c, &xcb_composite_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No composite extension");
		exit(1);
	}
	ps->composite_opcode = ext_info->major_opcode;
	ps->composite_event = ext_info->first_event;
	ps->composite_error = ext_info->first_error;

	{
		xcb_composite_query_version_reply_t *reply = xcb_composite_query_version_reply(
		    ps->c,
		    xcb_composite_query_version(ps->c, XCB_COMPOSITE_MAJOR_VERSION,
		                                XCB_COMPOSITE_MINOR_VERSION),
		    NULL);

		if (!reply || (reply->major_version == 0 && reply->minor_version < 2)) {
			log_fatal("Your X server doesn't have Composite >= 0.2 support, "
			          "compton cannot run.");
			exit(1);
		}
		free(reply);
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_damage_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No damage extension");
		exit(1);
	}
	ps->damage_event = ext_info->first_event;
	ps->damage_error = ext_info->first_error;
	xcb_discard_reply(ps->c, xcb_damage_query_version(ps->c, XCB_DAMAGE_MAJOR_VERSION,
	                                                  XCB_DAMAGE_MINOR_VERSION)
	                             .sequence);

	ext_info = xcb_get_extension_data(ps->c, &xcb_xfixes_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No XFixes extension");
		exit(1);
	}
	ps->xfixes_event = ext_info->first_event;
	ps->xfixes_error = ext_info->first_error;
	xcb_discard_reply(ps->c, xcb_xfixes_query_version(ps->c, XCB_XFIXES_MAJOR_VERSION,
	                                                  XCB_XFIXES_MINOR_VERSION)
	                             .sequence);

	// Parse configuration file
	win_option_mask_t winopt_mask[NUM_WINTYPES] = {{0}};
	bool shadow_enabled = false, fading_enable = false, hasneg = false;
	char *config_file_to_free = NULL;
	config_file = config_file_to_free = parse_config(
	    &ps->o, config_file, &shadow_enabled, &fading_enable, &hasneg, winopt_mask);

	if (IS_ERR(config_file_to_free)) {
		return NULL;
	}

	// Parse all of the rest command line options
	get_cfg(&ps->o, argc, argv, shadow_enabled, fading_enable, hasneg, winopt_mask);

	if (ps->o.logpath) {
		auto l = file_logger_new(ps->o.logpath);
		if (l) {
			log_info("Switching to log file: %s", ps->o.logpath);
			if (stderr_logger) {
				log_remove_target_tls(stderr_logger);
				stderr_logger = NULL;
			}
			log_add_target_tls(l);
			stderr_logger = NULL;
		} else {
			log_error("Failed to setup log file %s, I will keep using stderr",
			          ps->o.logpath);
		}
	}

	// Get needed atoms for c2 condition lists
	if (!(c2_list_postprocess(ps, ps->o.unredir_if_possible_blacklist) &&
	      c2_list_postprocess(ps, ps->o.paint_blacklist) &&
	      c2_list_postprocess(ps, ps->o.shadow_blacklist) &&
	      c2_list_postprocess(ps, ps->o.fade_blacklist) &&
	      c2_list_postprocess(ps, ps->o.blur_background_blacklist) &&
	      c2_list_postprocess(ps, ps->o.invert_color_list) &&
	      c2_list_postprocess(ps, ps->o.opacity_rules) &&
	      c2_list_postprocess(ps, ps->o.focus_blacklist))) {
		log_error("Post-processing of conditionals failed, some of your rules "
		          "might not work");
	}

	ps->gaussian_map = gaussian_kernel(ps->o.shadow_radius);
	sum_kernel_preprocess(ps->gaussian_map);

	rebuild_shadow_exclude_reg(ps);

	// Query X Shape
	ext_info = xcb_get_extension_data(ps->c, &xcb_shape_id);
	if (ext_info && ext_info->present) {
		ps->shape_event = ext_info->first_event;
		ps->shape_error = ext_info->first_error;
		ps->shape_exists = true;
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_randr_id);
	if (ext_info && ext_info->present) {
		ps->randr_exists = true;
		ps->randr_event = ext_info->first_event;
		ps->randr_error = ext_info->first_error;
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_present_id);
	if (ext_info && ext_info->present) {
		auto r = xcb_present_query_version_reply(
		    ps->c,
		    xcb_present_query_version(ps->c, XCB_PRESENT_MAJOR_VERSION,
		                              XCB_PRESENT_MINOR_VERSION),
		    NULL);
		if (r) {
			ps->present_exists = true;
			free(r);
		}
	}

	// Query X Sync
	ext_info = xcb_get_extension_data(ps->c, &xcb_sync_id);
	if (ext_info && ext_info->present) {
		ps->xsync_error = ext_info->first_error;
		ps->xsync_event = ext_info->first_event;
		// Need X Sync 3.1 for fences
		auto r = xcb_sync_initialize_reply(
		    ps->c,
		    xcb_sync_initialize(ps->c, XCB_SYNC_MAJOR_VERSION, XCB_SYNC_MINOR_VERSION),
		    NULL);
		if (r && (r->major_version > 3 ||
		          (r->major_version == 3 && r->minor_version >= 1))) {
			ps->xsync_exists = true;
			free(r);
		}
	}

	ps->sync_fence = XCB_NONE;
	if (!ps->xsync_exists && ps->o.xrender_sync_fence) {
		log_error("XSync extension not found. No XSync fence sync is "
		          "possible. (xrender-sync-fence can't be enabled)");
		ps->o.xrender_sync_fence = false;
	}

	if (ps->o.xrender_sync_fence) {
		ps->sync_fence = xcb_generate_id(ps->c);
		e = xcb_request_check(
		    ps->c, xcb_sync_create_fence(ps->c, ps->root, ps->sync_fence, 0));
		if (e) {
			log_error("Failed to create a XSync fence. xrender-sync-fence "
			          "will be disabled");
			ps->o.xrender_sync_fence = false;
			ps->sync_fence = XCB_NONE;
			free(e);
		}
	}

	// Query X RandR
	if ((ps->o.sw_opti && !ps->o.refresh_rate) || ps->o.xinerama_shadow_crop) {
		if (!ps->randr_exists) {
			log_fatal("No XRandR extension. sw-opti, refresh-rate or "
			          "xinerama-shadow-crop "
			          "cannot be enabled.");
			exit(1);
		}
	}

	// Query X Xinerama extension
	if (ps->o.xinerama_shadow_crop) {
		ext_info = xcb_get_extension_data(ps->c, &xcb_xinerama_id);
		ps->xinerama_exists = ext_info && ext_info->present;
	}

	rebuild_screen_reg(ps);

	// Overlay must be initialized before double buffer, and before creation
	// of OpenGL context.
	init_overlay(ps);

	// Initialize filters, must be preceded by OpenGL context creation
	if (!ps->o.experimental_backends && !init_render(ps)) {
		log_fatal("Failed to initialize the backend");
		exit(1);
	}

	if (ps->o.print_diagnostics) {
		print_diagnostics(ps, config_file);
		free(config_file_to_free);
		exit(0);
	}
	free(config_file_to_free);

	if (bkend_use_glx(ps) && !ps->o.experimental_backends) {
		auto gl_logger = gl_string_marker_logger_new();
		if (gl_logger) {
			log_info("Enabling gl string marker");
			log_add_target_tls(gl_logger);
		}
	}

	if (ps->o.experimental_backends) {
		if (ps->o.monitor_repaint && !backend_list[ps->o.backend]->fill) {
			log_warn("--monitor-repaint is not supported by the backend, "
			         "disabling");
			ps->o.monitor_repaint = false;
		}
	}

	// Initialize software optimization
	if (ps->o.sw_opti)
		ps->o.sw_opti = swopti_init(ps);

	// Monitor screen changes if vsync_sw is enabled and we are using
	// an auto-detected refresh rate, or when Xinerama features are enabled
	if (ps->randr_exists &&
	    ((ps->o.sw_opti && !ps->o.refresh_rate) || ps->o.xinerama_shadow_crop))
		xcb_randr_select_input(ps->c, ps->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

	cxinerama_upd_scrs(ps);

	// Create registration window
	if (!ps->reg_win && !register_cm(ps))
		exit(1);

	init_atoms(ps);

	{
		xcb_render_create_picture_value_list_t pa = {
		    .subwindowmode = IncludeInferiors,
		};

		ps->root_picture = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, ps->root, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		if (ps->overlay != XCB_NONE) {
			ps->tgt_picture = x_create_picture_with_visual_and_pixmap(
			    ps->c, ps->vis, ps->overlay, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		} else
			ps->tgt_picture = ps->root_picture;
	}

	ev_io_init(&ps->xiow, x_event_callback, ConnectionNumber(ps->dpy), EV_READ);
	ev_io_start(ps->loop, &ps->xiow);
	ev_init(&ps->unredir_timer, tmout_unredir_callback);
	if (ps->o.sw_opti)
		ev_idle_init(&ps->draw_idle, delayed_draw_callback);
	else
		ev_idle_init(&ps->draw_idle, draw_callback);

	ev_init(&ps->fade_timer, fade_timer_callback);
	ev_init(&ps->delayed_draw_timer, delayed_draw_timer_callback);

	// Set up SIGUSR1 signal handler to reset program
	ev_signal_init(&ps->usr1_signal, reset_enable, SIGUSR1);
	ev_signal_init(&ps->int_signal, exit_enable, SIGINT);
	ev_signal_start(ps->loop, &ps->usr1_signal);
	ev_signal_start(ps->loop, &ps->int_signal);

	// xcb can read multiple events from the socket when a request with reply is
	// made.
	//
	// Use an ev_prepare to make sure we cannot accidentally forget to handle them
	// before we go to sleep.
	//
	// If we don't drain the queue before goes to sleep (i.e. blocking on socket
	// input), we will be sleeping with events available in queue. Which might
	// cause us to block indefinitely because arrival of new events could be
	// dependent on processing of existing events (e.g. if we don't process damage
	// event and do damage subtract, new damage event won't be generated).
	//
	// So we make use of a ev_prepare handle, which is called right before libev
	// goes into sleep, to handle all the queued X events.
	ev_prepare_init(&ps->event_check, handle_queued_x_events);
	// Make sure nothing can cause xcb to read from the X socket after events are
	// handled and before we going to sleep.
	ev_set_priority(&ps->event_check, EV_MINPRI);
	ev_prepare_start(ps->loop, &ps->event_check);

	xcb_grab_server(ps->c);

	// Initialize DBus. We need to do this early, because add_win might call dbus
	// functions
	if (ps->o.dbus) {
#ifdef CONFIG_DBUS
		cdbus_init(ps, DisplayString(ps->dpy));
		if (!ps->dbus_data) {
			ps->o.dbus = false;
		}
#else
		log_fatal("DBus support not compiled in!");
		exit(1);
#endif
	}

	{
		xcb_window_t *children;
		int nchildren;

		xcb_query_tree_reply_t *reply =
		    xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, ps->root), NULL);

		if (reply) {
			children = xcb_query_tree_children(reply);
			nchildren = xcb_query_tree_children_length(reply);
		} else {
			children = NULL;
			nchildren = 0;
		}

		for (int i = 0; i < nchildren; i++) {
			add_win(ps, children[i], i ? children[i - 1] : XCB_NONE);
		}

		HASH_ITER2(ps->windows, w) {
			if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
				map_win(ps, w);
			}
		}

		free(reply);
		log_trace("Initial stack:");
		for (win *c = ps->window_stack; c; c = c->next) {
			log_trace("%#010x \"%s\"", c->id, c->name);
		}
	}

	if (ps->o.track_focus) {
		recheck_focus(ps);
	}

	e = xcb_request_check(ps->c, xcb_ungrab_server(ps->c));
	if (e) {
		log_error("Failed to ungrad server");
		free(e);
	}

	write_pid(ps);

	if (fork && stderr_logger) {
		// Remove the stderr logger if we will fork
		log_remove_target_tls(stderr_logger);
	}
	return ps;
}

/**
 * Destroy a session.
 *
 * Does not close the X connection or free the <code>session_t</code>
 * structure, though.
 *
 * @param ps session to destroy
 */
static void session_destroy(session_t *ps) {
	if (ps->redirected) {
		redir_stop(ps);
	}

	// Stop listening to events on root window
	xcb_change_window_attributes(ps->c, ps->root, XCB_CW_EVENT_MASK,
	                             (const uint32_t[]){0});

#ifdef CONFIG_DBUS
	// Kill DBus connection
	if (ps->o.dbus) {
		assert(ps->dbus_data);
		cdbus_destroy(ps);
	}
#endif

	// Free window linked list

	WIN_STACK_ITER(ps, w) {
		if (w->state != WSTATE_DESTROYING) {
			win_ev_stop(ps, w);
			HASH_DEL(ps->windows, w);
		}

		free_win_res(ps, w);
		free(w);
	}
	ps->window_stack = NULL;

	// Free blacklists
	free_wincondlst(&ps->o.shadow_blacklist);
	free_wincondlst(&ps->o.fade_blacklist);
	free_wincondlst(&ps->o.focus_blacklist);
	free_wincondlst(&ps->o.invert_color_list);
	free_wincondlst(&ps->o.blur_background_blacklist);
	free_wincondlst(&ps->o.opacity_rules);
	free_wincondlst(&ps->o.paint_blacklist);
	free_wincondlst(&ps->o.unredir_if_possible_blacklist);

	// Free tracked atom list
	{
		latom_t *next = NULL;
		for (latom_t *this = ps->track_atom_lst; this; this = next) {
			next = this->next;
			free(this);
		}

		ps->track_atom_lst = NULL;
	}

	// Free ignore linked list
	{
		ignore_t *next = NULL;
		for (ignore_t *ign = ps->ignore_head; ign; ign = next) {
			next = ign->next;

			free(ign);
		}

		// Reset head and tail
		ps->ignore_head = NULL;
		ps->ignore_tail = &ps->ignore_head;
	}

	// Free tgt_{buffer,picture} and root_picture
	if (ps->tgt_buffer.pict == ps->tgt_picture)
		ps->tgt_buffer.pict = XCB_NONE;

	if (ps->tgt_picture == ps->root_picture)
		ps->tgt_picture = XCB_NONE;
	else
		free_picture(ps->c, &ps->tgt_picture);

	free_picture(ps->c, &ps->root_picture);
	free_paint(ps, &ps->tgt_buffer);

	pixman_region32_fini(&ps->screen_reg);
	free(ps->expose_rects);

	free(ps->o.write_pid_path);
	free(ps->o.logpath);
	for (int i = 0; i < MAX_BLUR_PASS; ++i) {
		free(ps->o.blur_kerns[i]);
		free(ps->blur_kerns_cache[i]);
	}
	free(ps->o.glx_fshader_win_str);
	free_xinerama_info(ps);

#ifdef CONFIG_VSYNC_DRM
	// Close file opened for DRM VSync
	if (ps->drm_fd >= 0) {
		close(ps->drm_fd);
		ps->drm_fd = -1;
	}
#endif

	// Release overlay window
	if (ps->overlay) {
		xcb_composite_release_overlay_window(ps->c, ps->overlay);
		ps->overlay = XCB_NONE;
	}

	if (ps->sync_fence) {
		xcb_sync_destroy_fence(ps->c, ps->sync_fence);
		ps->sync_fence = XCB_NONE;
	}

	// Free reg_win
	if (ps->reg_win) {
		xcb_destroy_window(ps->c, ps->reg_win);
		ps->reg_win = XCB_NONE;
	}

	if (ps->o.experimental_backends) {
		// backend is deinitialized in redir_stop
		assert(ps->backend_data == NULL);
	} else {
		deinit_render(ps);
	}

	// Flush all events
	x_sync(ps->c);
	ev_io_stop(ps->loop, &ps->xiow);
	free_conv(ps->gaussian_map);

#ifdef DEBUG_XRC
	// Report about resource leakage
	xrc_report_xid();
#endif

	// Stop libev event handlers
	ev_timer_stop(ps->loop, &ps->unredir_timer);
	ev_timer_stop(ps->loop, &ps->fade_timer);
	ev_idle_stop(ps->loop, &ps->draw_idle);
	ev_prepare_stop(ps->loop, &ps->event_check);
	ev_signal_stop(ps->loop, &ps->usr1_signal);
	ev_signal_stop(ps->loop, &ps->int_signal);

	log_deinit_tls();
}

/**
 * Do the actual work.
 *
 * @param ps current session
 */
static void session_run(session_t *ps) {
	if (ps->o.sw_opti)
		ps->paint_tm_offset = get_time_timeval().tv_usec;

	// In benchmark mode, we want draw_idle handler to always be active
	if (ps->o.benchmark) {
		ev_idle_start(ps->loop, &ps->draw_idle);
	} else {
		// Let's draw our first frame!
		queue_redraw(ps);
	}
	ev_run(ps->loop, 0);
}

/**
 * The function that everybody knows.
 */
int main(int argc, char **argv) {
	// Set locale so window names with special characters are interpreted
	// correctly
	setlocale(LC_ALL, "");
	log_init_tls();

	int exit_code;
	char *config_file = NULL;
	bool all_xerrors = false, need_fork = false;
	if (get_early_config(argc, argv, &config_file, &all_xerrors, &need_fork, &exit_code)) {
		return exit_code;
	}

	int pfds[2];
	if (need_fork) {
		if (pipe2(pfds, O_CLOEXEC)) {
			perror("pipe2");
			return 1;
		}
		auto pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid > 0) {
			// We are the parent
			close(pfds[1]);
			// We wait for the child to tell us it has finished initialization
			// by sending us something via the pipe.
			int tmp;
			if (read(pfds[0], &tmp, sizeof tmp) <= 0) {
				// Failed to read, the child has most likely died
				// We can probably waitpid() here.
				return 1;
			} else {
				// We are done
				return 0;
			}
		}
		// We are the child
		close(pfds[0]);
	}

	// Main loop
	bool quit = false;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "Can't open display.");
		return 1;
	}
	XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

	do {
		ps_g = session_init(argc, argv, dpy, config_file, all_xerrors, need_fork);
		if (!ps_g) {
			log_fatal("Failed to create new compton session.");
			return 1;
		}
		if (need_fork) {
			// Finishing up daemonization
			// Close files
			if (fclose(stdout) || fclose(stderr) || fclose(stdin)) {
				log_fatal("Failed to close standard input/output");
				return 1;
			}
			// Make us the session and process group leader so we don't get
			// killed when our parent die.
			setsid();
			// Notify the parent that we are done. This might cause the parent
			// to quit, so only do this after setsid()
			int tmp = 1;
			write(pfds[1], &tmp, sizeof tmp);
			close(pfds[1]);
			// We only do this once
			need_fork = false;
		}
		session_run(ps_g);
		quit = ps_g->quit;
		session_destroy(ps_g);
		free(ps_g);
		ps_g = NULL;
	} while (!quit);

	if (dpy) {
		XCloseDisplay(dpy);
	}
	free(config_file);

	return 0;
}
