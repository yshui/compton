// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xinerama.h>

#include "atom.h"
#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "compton.h"
#include "config.h"
#include "list.h"
#include "log.h"
#include "region.h"
#include "string_utils.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "x.h"

#ifdef CONFIG_DBUS
#include "dbus.h"
#endif

#include "win.h"

// TODO Make more window states internal
struct managed_win_internal {
	struct managed_win base;

	/// A bit mask of unhandled window updates
	uint_fast32_t pending_updates;
};

#define OPAQUE (0xffffffff)
static const int WIN_GET_LEADER_MAX_RECURSION = 20;
static const int ROUNDED_PIXELS = 1;
static const double ROUNDED_PERCENT = 0.05;

/// Generate a "return by value" function, from a function that returns the
/// region via a region_t pointer argument.
/// Function signature has to be (win *, region_t *)
#define gen_by_val(fun)                                                                  \
	region_t fun##_by_val(const struct managed_win *w) {                             \
		region_t ret;                                                            \
		pixman_region32_init(&ret);                                              \
		fun(w, &ret);                                                            \
		return ret;                                                              \
	}

/**
 * Clear leader cache of all windows.
 */
static inline void clear_cache_win_leaders(session_t *ps) {
	win_stack_foreach_managed(w, &ps->window_stack) {
		w->cache_leader = XCB_NONE;
	}
}

static xcb_window_t win_get_leader_raw(session_t *ps, struct managed_win *w, int recursions);

/**
 * Get the leader of a window.
 *
 * This function updates w->cache_leader if necessary.
 */
static inline xcb_window_t win_get_leader(session_t *ps, struct managed_win *w) {
	return win_get_leader_raw(ps, w, 0);
}

/**
 * Update focused state of a window.
 */
static void win_update_focused(session_t *ps, struct managed_win *w) {
	if (UNSET != w->focused_force) {
		w->focused = w->focused_force;
	} else {
		w->focused = win_is_focused_real(ps, w);

		// Use wintype_focus, and treat WM windows and override-redirected
		// windows specially
		if (ps->o.wintype_option[w->window_type].focus ||
		    (ps->o.mark_wmwin_focused && w->wmwin) ||
		    (ps->o.mark_ovredir_focused && w->base.id == w->client_win && !w->wmwin) ||
		    (w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
		     c2_match(ps, w, ps->o.focus_blacklist, NULL)))
			w->focused = true;

		// If window grouping detection is enabled, mark the window active if
		// its group is
		if (ps->o.track_leader && ps->active_leader &&
		    win_get_leader(ps, w) == ps->active_leader) {
			w->focused = true;
		}
	}

	// Always recalculate the window target opacity, since some opacity-related
	// options depend on the output value of win_is_focused_real() instead of
	// w->focused
	auto opacity_target_old = w->opacity_target;
	w->opacity_target = win_calc_opacity_target(ps, w, false);
	if (opacity_target_old != w->opacity_target && w->state == WSTATE_MAPPED) {
		// Only MAPPED can transition to FADING
		w->state = WSTATE_FADING;
		if (!ps->redirected) {
			CHECK(!win_skip_fading(ps, w));
		}
	}
}

/**
 * Run win_update_focused() on all windows with the same leader window.
 *
 * @param leader leader window ID
 */
static inline void group_update_focused(session_t *ps, xcb_window_t leader) {
	if (!leader)
		return;

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}
		auto mw = (struct managed_win *)w;
		if (win_get_leader(ps, mw) == leader) {
			win_update_focused(ps, mw);
		}
	}

	return;
}

static inline const char *win_get_name_if_managed(const struct win *w) {
	if (!w->managed) {
		return "(unmanaged)";
	}
	auto mw = (struct managed_win *)w;
	return mw->name;
}

/**
 * Return whether a window group is really focused.
 *
 * @param leader leader window ID
 * @return true if the window group is focused, false otherwise
 */
static inline bool group_is_focused(session_t *ps, xcb_window_t leader) {
	if (!leader) {
		return false;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}
		auto mw = (struct managed_win *)w;
		if (win_get_leader(ps, mw) == leader && win_is_focused_real(ps, mw)) {
			return true;
		}
	}

	return false;
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static void win_get_region_local(const struct managed_win *w, region_t *res) {
	assert(w->widthb >= 0 && w->heightb >= 0);
	pixman_region32_fini(res);
	pixman_region32_init_rect(res, 0, 0, (uint)w->widthb, (uint)w->heightb);
}

/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
void win_get_region_noframe_local(const struct managed_win *w, region_t *res) {
	const margin_t extents = win_calc_frame_extents(w);

	int x = extents.left;
	int y = extents.top;
	int width = max2(w->g.width - (extents.left + extents.right), 0);
	int height = max2(w->g.height - (extents.top + extents.bottom), 0);

	pixman_region32_fini(res);
	if (width > 0 && height > 0) {
		pixman_region32_init_rect(res, x, y, (uint)width, (uint)height);
	}
}

void win_get_region_frame_local(const struct managed_win *w, region_t *res) {
	const margin_t extents = win_calc_frame_extents(w);
	pixman_region32_fini(res);
	pixman_region32_init_rects(
	    res,
	    (rect_t[]){
	        // top
	        {.x1 = 0, .y1 = 0, .x2 = w->g.width, .y2 = extents.top},
	        // bottom
	        {.x1 = 0, .y1 = w->g.height - extents.bottom, .x2 = w->g.width, .y2 = w->g.height},
	        // left
	        {.x1 = 0, .y1 = 0, .x2 = extents.left, .y2 = w->g.height},
	        // right
	        {.x1 = w->g.width - extents.right, .y1 = 0, .x2 = w->g.width, .y2 = w->g.height},
	    },
	    4);

	// limit the frame region to inside the window
	region_t reg_win;
	pixman_region32_init_rect(&reg_win, 0, 0, w->g.width, w->g.height);
	pixman_region32_intersect(res, &reg_win, res);
	pixman_region32_fini(&reg_win);
}

gen_by_val(win_get_region_frame_local);

/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, const struct managed_win *w) {
	// XXX there was a cached extents region, investigate
	//     if that's better
	region_t extents;
	pixman_region32_init(&extents);
	win_extents(w, &extents);
	add_damage(ps, &extents);
	pixman_region32_fini(&extents);
}

/// Release the images attached to this window
static inline void win_release_pixmap(backend_t *base, struct managed_win *w) {
	log_debug("Releasing pixmap of window %#010x (%s)", w->base.id, w->name);
	assert(w->win_image);
	if (w->win_image) {
		base->ops->release_image(base, w->win_image);
		w->win_image = NULL;
		w->flags |= WIN_FLAGS_PIXMAP_NONE;
	}
}
static inline void win_release_shadow(backend_t *base, struct managed_win *w) {
	log_debug("Releasing shadow of window %#010x (%s)", w->base.id, w->name);
	assert(w->shadow_image);
	if (w->shadow_image) {
		base->ops->release_image(base, w->shadow_image);
		w->shadow_image = NULL;
		w->flags |= WIN_FLAGS_SHADOW_NONE;
	}
}

static inline bool win_bind_pixmap(struct backend_base *b, struct managed_win *w) {
	assert(!w->win_image);
	auto pixmap = x_new_id(b->c);
	auto e = xcb_request_check(
	    b->c, xcb_composite_name_window_pixmap_checked(b->c, w->base.id, pixmap));
	if (e) {
		log_error("Failed to get named pixmap for window %#010x(%s)", w->base.id,
		          w->name);
		free(e);
		return false;
	}
	log_debug("New named pixmap for %#010x (%s) : %#010x", w->base.id, w->name, pixmap);
	w->win_image =
	    b->ops->bind_pixmap(b, pixmap, x_get_visual_info(b->c, w->a.visual), true);
	if (!w->win_image) {
		log_error("Failed to bind pixmap");
		w->flags |= WIN_FLAGS_IMAGE_ERROR;
		return false;
	}

	w->flags &= ~WIN_FLAGS_PIXMAP_NONE;
	return true;
}

bool win_bind_shadow(struct backend_base *b, struct managed_win *w, struct color c,
                     struct conv *kernel) {
	assert(!w->shadow_image);
	assert(w->shadow);
	w->shadow_image = b->ops->render_shadow(b, w->widthb, w->heightb, kernel, c.red,
	                                        c.green, c.blue, c.alpha);
	if (!w->shadow_image) {
		log_error("Failed to bind shadow image");
		w->flags |= WIN_FLAGS_IMAGE_ERROR;
		return false;
	}

	log_debug("New shadow for %#010x (%s)", w->base.id, w->name);
	w->flags &= ~WIN_FLAGS_SHADOW_NONE;
	return true;
}

void win_release_images(struct backend_base *backend, struct managed_win *w) {
	// we don't want to consider what we should do if the image we want to release is
	// stale (do we clear the stale flags or not?)
	assert((w->flags & WIN_FLAGS_IMAGES_STALE) == 0);

	if ((w->flags & WIN_FLAGS_PIXMAP_NONE) == 0) {
		win_release_pixmap(backend, w);
	}

	if ((w->flags & WIN_FLAGS_SHADOW_NONE) == 0) {
		win_release_shadow(backend, w);
	}
}

void win_process_flags(session_t *ps, struct managed_win *w) {
	// Make sure all pending window updates are processed before this. Making this
	// assumption simplifies some checks (e.g. whether window is mapped)
	auto iw = (struct managed_win_internal *)w;
	assert(iw->pending_updates == 0);

	if (!w->flags || (w->flags & WIN_FLAGS_IMAGE_ERROR) != 0) {
		return;
	}

	// Not a loop
	while ((w->flags & WIN_FLAGS_IMAGES_STALE) != 0) {
		// Image needs to be updated, update it.
		if (!ps->backend_data) {
			// We are using legacy backend, nothing to do here.
			break;
		}

		if ((w->flags & WIN_FLAGS_PIXMAP_STALE) != 0) {
			// Check to make sure the window is still mapped, otherwise we
			// won't be able to rebind pixmap after releasing it, yet we might
			// still need the pixmap for rendering.
			assert(w->state != WSTATE_UNMAPPING && w->state != WSTATE_DESTROYING);
			if ((w->flags & WIN_FLAGS_PIXMAP_NONE) == 0) {
				// Must release images first, otherwise breaks
				// NVIDIA driver
				win_release_pixmap(ps->backend_data, w);
			}
			win_bind_pixmap(ps->backend_data, w);
		}

		if ((w->flags & WIN_FLAGS_SHADOW_STALE) != 0) {
			if ((w->flags & WIN_FLAGS_SHADOW_NONE) == 0) {
				win_release_shadow(ps->backend_data, w);
			}
			if (w->shadow) {
				win_bind_shadow(ps->backend_data, w,
				                (struct color){.red = ps->o.shadow_red,
				                               .green = ps->o.shadow_green,
				                               .blue = ps->o.shadow_blue,
				                               .alpha = ps->o.shadow_opacity},
				                ps->gaussian_map);
			}
		}

		// break here, loop always run only once
		break;
	}

	// Clear stale image flags
	w->flags &= ~WIN_FLAGS_IMAGES_STALE;
}

/**
 * Check if a window has rounded corners.
 * XXX This is really dumb
 */
static bool attr_pure win_has_rounded_corners(const struct managed_win *w) {
	if (!w->bounding_shaped) {
		return false;
	}

	// Quit if border_size() returns XCB_NONE
	if (!pixman_region32_not_empty((region_t *)&w->bounding_shape)) {
		return false;
	}

	// Determine the minimum width/height of a rectangle that could mark
	// a window as having rounded corners
	auto minwidth =
	    (uint16_t)max2(w->widthb * (1 - ROUNDED_PERCENT), w->widthb - ROUNDED_PIXELS);
	auto minheight =
	    (uint16_t)max2(w->heightb * (1 - ROUNDED_PERCENT), w->heightb - ROUNDED_PIXELS);

	// Get the rectangles in the bounding region
	int nrects = 0;
	const rect_t *rects =
	    pixman_region32_rectangles((region_t *)&w->bounding_shape, &nrects);

	// Look for a rectangle large enough for this window be considered
	// having rounded corners
	for (int i = 0; i < nrects; ++i) {
		if (rects[i].x2 - rects[i].x1 >= minwidth &&
		    rects[i].y2 - rects[i].y1 >= minheight) {
			return true;
		}
	}
	return false;
}

int win_update_name(session_t *ps, struct managed_win *w) {
	XTextProperty text_prop = {NULL, XCB_NONE, 0, 0};
	char **strlst = NULL;
	int nstr = 0;

	if (!w->client_win)
		return 0;

	if (!(wid_get_text_prop(ps, w->client_win, ps->atoms->a_NET_WM_NAME, &strlst, &nstr))) {
		log_trace("(%#010x): _NET_WM_NAME unset, falling back to WM_NAME.",
		          w->client_win);

		if (!(XGetWMName(ps->dpy, w->client_win, &text_prop) && text_prop.value)) {
			return -1;
		}
		if (Success != XmbTextPropertyToTextList(ps->dpy, &text_prop, &strlst, &nstr) ||
		    !nstr || !strlst) {
			if (strlst)
				XFreeStringList(strlst);
			XFree(text_prop.value);
			return -1;
		}
		XFree(text_prop.value);
	}

	int ret = 0;
	if (!w->name || strcmp(w->name, strlst[0]) != 0) {
		ret = 1;
		free(w->name);
		w->name = strdup(strlst[0]);
	}

	XFreeStringList(strlst);

	log_trace("(%#010x): client = %#010x, name = \"%s\", "
	          "ret = %d",
	          w->base.id, w->client_win, w->name, ret);
	return ret;
}

int win_get_role(session_t *ps, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	if (!wid_get_text_prop(ps, w->client_win, ps->atoms->aWM_WINDOW_ROLE, &strlst, &nstr))
		return -1;

	int ret = 0;
	if (!w->role || strcmp(w->role, strlst[0]) != 0) {
		ret = 1;
		free(w->role);
		w->role = strdup(strlst[0]);
	}

	XFreeStringList(strlst);

	log_trace("(%#010x): client = %#010x, role = \"%s\", "
	          "ret = %d",
	          w->base.id, w->client_win, w->role, ret);
	return ret;
}

/**
 * Check if a window is bounding-shaped.
 */
static inline bool win_bounding_shaped(const session_t *ps, xcb_window_t wid) {
	if (ps->shape_exists) {
		xcb_shape_query_extents_reply_t *reply;
		Bool bounding_shaped;

		reply = xcb_shape_query_extents_reply(
		    ps->c, xcb_shape_query_extents(ps->c, wid), NULL);
		bounding_shaped = reply && reply->bounding_shaped;
		free(reply);

		return bounding_shaped;
	}

	return false;
}

static wintype_t wid_get_prop_wintype(session_t *ps, xcb_window_t wid) {
	winprop_t prop =
	    wid_get_prop(ps, wid, ps->atoms->a_NET_WM_WINDOW_TYPE, 32L, XCB_ATOM_ATOM, 32);

	for (unsigned i = 0; i < prop.nitems; ++i) {
		for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
			if (ps->atoms_wintypes[j] == (xcb_atom_t)prop.p32[i]) {
				free_winprop(&prop);
				return j;
			}
		}
	}

	free_winprop(&prop);

	return WINTYPE_UNKNOWN;
}

static bool
wid_get_opacity_prop(session_t *ps, xcb_window_t wid, opacity_t def, opacity_t *out) {
	bool ret = false;
	*out = def;

	winprop_t prop = wid_get_prop(ps, wid, ps->atoms->a_NET_WM_WINDOW_OPACITY, 1L,
	                              XCB_ATOM_CARDINAL, 32);

	if (prop.nitems) {
		*out = *prop.c32;
		ret = true;
	}

	free_winprop(&prop);

	return ret;
}

// XXX should distinguish between frame has alpha and window body has alpha
bool win_has_alpha(const struct managed_win *w) {
	return w->pictfmt && w->pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->pictfmt->direct.alpha_mask;
}

bool win_client_has_alpha(const struct managed_win *w) {
	return w->client_pictfmt && w->client_pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->client_pictfmt->direct.alpha_mask;
}

winmode_t win_calc_mode(const struct managed_win *w) {
	if (w->opacity < 1.0) {
		return WMODE_TRANS;
	}
	if (w->frame_opacity != 1.0 && win_has_frame(w)) {
		return WMODE_FRAME_TRANS;
	}

	if (win_has_alpha(w)) {
		// The WM window has alpha
		if (win_client_has_alpha(w)) {
			// The client window also has alpha, the entire window is
			// transparent
			return WMODE_TRANS;
		}
		if (win_has_frame(w)) {
			// The client window doesn't have alpha, but we have a WM frame
			// window, which has alpha.
			return WMODE_FRAME_TRANS;
		}
		// Although the WM window has alpha, the frame window has 0 size, so
		// consider the window solid
	}

	// log_trace("Window %#010x(%s) is solid", w->client_win, w->name);
	return WMODE_SOLID;
}

/**
 * Calculate and return the opacity target of a window.
 *
 * The priority of opacity settings are:
 *
 * inactive_opacity_override (if set, and unfocused) > _NET_WM_WINDOW_OPACITY (if set) >
 * opacity-rules (if matched) > window type default opacity > active/inactive opacity
 *
 * @param ps           current session
 * @param w            struct _win object representing the window
 * @param ignore_state whether window state should be ignored in opacity calculation
 *
 * @return target opacity
 */
double win_calc_opacity_target(session_t *ps, const struct managed_win *w, bool ignore_state) {
	double opacity = 1;

	if (w->state == WSTATE_UNMAPPED && !ignore_state) {
		// be consistent
		return 0;
	}
	if ((w->state == WSTATE_UNMAPPING || w->state == WSTATE_DESTROYING) && !ignore_state) {
		return 0;
	}
	// Try obeying opacity property and window type opacity firstly
	if (w->has_opacity_prop) {
		opacity = ((double)w->opacity_prop) / OPAQUE;
	} else if (w->opacity_is_set) {
		opacity = w->opacity_set;
	} else if (!safe_isnan(ps->o.wintype_option[w->window_type].opacity)) {
		opacity = ps->o.wintype_option[w->window_type].opacity;
	} else {
		// Respect active_opacity only when the window is physically focused
		if (win_is_focused_real(ps, w))
			opacity = ps->o.active_opacity;
		else if (!w->focused)
			// Respect inactive_opacity in some cases
			opacity = ps->o.inactive_opacity;
	}

	// respect inactive override
	if (ps->o.inactive_opacity_override && !w->focused) {
		opacity = ps->o.inactive_opacity;
	}

	return opacity;
}

/**
 * Determine whether a window is to be dimmed.
 */
bool win_should_dim(session_t *ps, const struct managed_win *w) {
	// Make sure we do nothing if the window is unmapped / being destroyed
	if (w->state == WSTATE_UNMAPPED) {
		return false;
	}

	if (ps->o.inactive_dim > 0 && !(w->focused)) {
		return true;
	} else {
		return false;
	}
}

/**
 * Determine if a window should fade on opacity change.
 */
bool win_should_fade(session_t *ps, const struct managed_win *w) {
	// To prevent it from being overwritten by last-paint value if the window is
	if (w->fade_force != UNSET) {
		return w->fade_force;
	}
	if (ps->o.no_fading_openclose && w->in_openclose) {
		return false;
	}
	if (ps->o.no_fading_destroyed_argb && w->state == WSTATE_DESTROYING &&
	    win_has_alpha(w) && w->client_win && w->client_win != w->base.id) {
		// deprecated
		return false;
	}
	// Ignore other possible causes of fading state changes after window
	// gets unmapped
	// if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
	//}
	if (c2_match(ps, w, ps->o.fade_blacklist, NULL)) {
		return false;
	}
	return ps->o.wintype_option[w->window_type].fade;
}

/**
 * Reread _COMPTON_SHADOW property from a window.
 *
 * The property must be set on the outermost window, usually the WM frame.
 */
void win_update_prop_shadow_raw(session_t *ps, struct managed_win *w) {
	winprop_t prop = wid_get_prop(ps, w->base.id, ps->atoms->a_COMPTON_SHADOW, 1,
	                              XCB_ATOM_CARDINAL, 32);

	if (!prop.nitems) {
		w->prop_shadow = -1;
	} else {
		w->prop_shadow = *prop.c32;
	}

	free_winprop(&prop);
}

static void win_set_shadow(session_t *ps, struct managed_win *w, bool shadow_new) {
	if (w->shadow == shadow_new) {
		return;
	}

	log_debug("Updating shadow property of window %#010x (%s) to %d", w->base.id,
	          w->name, shadow_new);

	region_t extents;
	pixman_region32_init(&extents);
	win_extents(w, &extents);

	w->shadow = shadow_new;

	// Window extents need update on shadow state change
	// Shadow geometry currently doesn't change on shadow state change
	// calc_shadow_geometry(ps, w);
	// Mark the old extents as damaged if the shadow is removed
	if (!w->shadow) {
		add_damage(ps, &extents);
	}

	pixman_region32_clear(&extents);
	// Mark the new extents as damaged if the shadow is added
	if (w->shadow) {
		win_extents(w, &extents);
		add_damage_from_win(ps, w);
		if (w->state != WSTATE_UNMAPPED) {
			assert(!w->shadow_image);
			// Delayed creation of shadow image
			w->flags |= WIN_FLAGS_SHADOW_STALE;
			ps->pending_updates = true;
		}
	}
	pixman_region32_fini(&extents);
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
static void win_determine_shadow(session_t *ps, struct managed_win *w) {
	log_debug("Determining shadow of window %#010x (%s)", w->base.id, w->name);
	bool shadow_new = w->shadow;

	if (w->shadow_force != UNSET) {
		shadow_new = w->shadow_force;
	} else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
		shadow_new = true;
		if (!ps->o.wintype_option[w->window_type].shadow) {
			log_debug("Shadow disabled by wintypes");
			shadow_new = false;
		} else if (c2_match(ps, w, ps->o.shadow_blacklist, NULL)) {
			log_debug("Shadow disabled by shadow-exclude");
			shadow_new = false;
		} else if (ps->o.shadow_ignore_shaped && w->bounding_shaped &&
		           !w->rounded_corners) {
			log_debug("Shadow disabled by shadow-ignore-shaped");
			shadow_new = false;
		} else if (ps->o.respect_prop_shadow && w->prop_shadow == 0) {
			log_debug("Shadow disabled by shadow property");
			shadow_new = false;
		}
	}

	win_set_shadow(ps, w, shadow_new);
}

/**
 * Reread _COMPTON_SHADOW property from a window and update related
 * things.
 */
void win_update_prop_shadow(session_t *ps, struct managed_win *w) {
	long attr_shadow_old = w->prop_shadow;

	win_update_prop_shadow_raw(ps, w);

	if (w->prop_shadow != attr_shadow_old)
		win_determine_shadow(ps, w);
}

static void win_set_invert_color(session_t *ps, struct managed_win *w, bool invert_color_new) {
	if (w->invert_color == invert_color_new)
		return;

	w->invert_color = invert_color_new;

	add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have color inverted.
 */
static void win_determine_invert_color(session_t *ps, struct managed_win *w) {
	bool invert_color_new = w->invert_color;

	if (UNSET != w->invert_color_force)
		invert_color_new = w->invert_color_force;
	else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE)
		invert_color_new = c2_match(ps, w, ps->o.invert_color_list, NULL);

	win_set_invert_color(ps, w, invert_color_new);
}

/**
 * Set w->invert_color_force of a window.
 */
void win_set_invert_color_force(session_t *ps, struct managed_win *w, switch_t val) {
	if (val != w->invert_color_force) {
		w->invert_color_force = val;
		win_determine_invert_color(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Set w->fade_force of a window.
 *
 * Doesn't affect fading already in progress
 */
void win_set_fade_force(struct managed_win *w, switch_t val) {
	w->fade_force = val;
}

/**
 * Set w->focused_force of a window.
 */
void win_set_focused_force(session_t *ps, struct managed_win *w, switch_t val) {
	if (val != w->focused_force) {
		w->focused_force = val;
		win_update_focused(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Set w->shadow_force of a window.
 */
void win_set_shadow_force(session_t *ps, struct managed_win *w, switch_t val) {
	if (val != w->shadow_force) {
		w->shadow_force = val;
		win_determine_shadow(ps, w);
		queue_redraw(ps);
	}
}

static void
win_set_blur_background(session_t *ps, struct managed_win *w, bool blur_background_new) {
	if (w->blur_background == blur_background_new)
		return;

	w->blur_background = blur_background_new;

	// This damage might not be absolutely necessary (e.g. when the window is opaque),
	// but blur_background changes should be rare, so this should be fine.
	add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have background blurred.
 */
static void win_determine_blur_background(session_t *ps, struct managed_win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
		return;

	bool blur_background_new =
	    ps->o.blur_method && !c2_match(ps, w, ps->o.blur_background_blacklist, NULL);

	win_set_blur_background(ps, w, blur_background_new);
}

/**
 * Update window opacity according to opacity rules.
 */
void win_update_opacity_rule(session_t *ps, struct managed_win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	double opacity = 1.0;
	bool is_set = false;
	void *val = NULL;
	if (c2_match(ps, w, ps->o.opacity_rules, &val)) {
		opacity = ((double)(long)val) / 100.0;
		is_set = true;
	}

	w->opacity_set = opacity;
	w->opacity_is_set = is_set;
}

/**
 * Function to be called on window type changes.
 */
static void win_on_wtype_change(session_t *ps, struct managed_win *w) {
	win_determine_shadow(ps, w);
	win_update_focused(ps, w);
	if (ps->o.invert_color_list)
		win_determine_invert_color(ps, w);
	if (ps->o.opacity_rules)
		win_update_opacity_rule(ps, w);
}

/**
 * Function to be called on window data changes.
 *
 * TODO need better name
 */
void win_on_factor_change(session_t *ps, struct managed_win *w) {
	if (ps->o.shadow_blacklist)
		win_determine_shadow(ps, w);
	if (ps->o.invert_color_list)
		win_determine_invert_color(ps, w);
	if (ps->o.focus_blacklist)
		win_update_focused(ps, w);
	if (ps->o.blur_background_blacklist)
		win_determine_blur_background(ps, w);
	if (ps->o.opacity_rules)
		win_update_opacity_rule(ps, w);
	if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->o.paint_blacklist)
		w->paint_excluded = c2_match(ps, w, ps->o.paint_blacklist, NULL);
	if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->o.unredir_if_possible_blacklist)
		w->unredir_if_possible_excluded =
		    c2_match(ps, w, ps->o.unredir_if_possible_blacklist, NULL);
	w->reg_ignore_valid = false;
}

/**
 * Update cache data in struct _win that depends on window size.
 */
void win_on_win_size_change(session_t *ps, struct managed_win *w) {
	w->widthb = w->g.width + w->g.border_width * 2;
	w->heightb = w->g.height + w->g.border_width * 2;
	w->shadow_dx = ps->o.shadow_offset_x;
	w->shadow_dy = ps->o.shadow_offset_y;
	w->shadow_width = w->widthb + ps->o.shadow_radius * 2;
	w->shadow_height = w->heightb + ps->o.shadow_radius * 2;

	// Invalidate the shadow we built
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_MAPPING ||
	    w->state == WSTATE_FADING) {
		w->flags |= WIN_FLAGS_IMAGES_STALE;
		ps->pending_updates = true;
	} else {
		assert(w->state == WSTATE_UNMAPPED);
	}
}

/**
 * Update window type.
 */
void win_update_wintype(session_t *ps, struct managed_win *w) {
	const wintype_t wtype_old = w->window_type;

	// Detect window type here
	w->window_type = wid_get_prop_wintype(ps, w->client_win);

	// Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
	// override-redirect windows or windows without WM_TRANSIENT_FOR as
	// _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
	if (WINTYPE_UNKNOWN == w->window_type) {
		if (w->a.override_redirect ||
		    !wid_has_prop(ps, w->client_win, ps->atoms->aWM_TRANSIENT_FOR))
			w->window_type = WINTYPE_NORMAL;
		else
			w->window_type = WINTYPE_DIALOG;
	}

	if (w->window_type != wtype_old)
		win_on_wtype_change(ps, w);
}

/**
 * Mark a window as the client window of another.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 * @param client window ID of the client window
 */
void win_mark_client(session_t *ps, struct managed_win *w, xcb_window_t client) {
	w->client_win = client;

	// If the window isn't mapped yet, stop here, as the function will be
	// called in map_win()
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
		return;

	auto e = xcb_request_check(
	    ps->c, xcb_change_window_attributes(
	               ps->c, client, XCB_CW_EVENT_MASK,
	               (const uint32_t[]){determine_evmask(ps, client, WIN_EVMODE_CLIENT)}));
	if (e) {
		log_error("Failed to change event mask of window %#010x", client);
		free(e);
	}

	win_update_wintype(ps, w);

	// Get frame widths. The window is in damaged area already.
	if (ps->o.frame_opacity != 1)
		win_update_frame_extents(ps, w, client);

	// Get window group
	if (ps->o.track_leader)
		win_update_leader(ps, w);

	// Get window name and class if we are tracking them
	if (ps->o.track_wdata) {
		win_update_name(ps, w);
		win_get_class(ps, w);
		win_get_role(ps, w);
	}

	// Update everything related to conditions
	win_on_factor_change(ps, w);

	// Update window focus state
	win_update_focused(ps, w);

	auto r = xcb_get_window_attributes_reply(
	    ps->c, xcb_get_window_attributes(ps->c, w->client_win), NULL);
	if (!r) {
		log_error("Failed to get client window attributes");
		return;
	}

	w->client_pictfmt = x_get_pictform_for_visual(ps->c, r->visual);
	free(r);
}

/**
 * Unmark current client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_unmark_client(session_t *ps, struct managed_win *w) {
	xcb_window_t client = w->client_win;

	w->client_win = XCB_NONE;

	// Recheck event mask
	xcb_change_window_attributes(
	    ps->c, client, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, client, WIN_EVMODE_UNKNOWN)});
}

/**
 * Recheck client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
static void win_recheck_client(session_t *ps, struct managed_win *w) {
	// Initialize wmwin to false
	w->wmwin = false;

	// Look for the client window

	// Always recursively look for a window with WM_STATE, as Fluxbox
	// sets override-redirect flags on all frame windows.
	xcb_window_t cw = find_client_win(ps, w->base.id);
	if (cw) {
		log_trace("(%#010x): client %#010x", w->base.id, cw);
	}
	// Set a window's client window to itself if we couldn't find a
	// client window
	if (!cw) {
		cw = w->base.id;
		w->wmwin = !w->a.override_redirect;
		log_trace("(%#010x): client self (%s)", w->base.id,
		          (w->wmwin ? "wmwin" : "override-redirected"));
	}

	// Unmark the old one
	if (w->client_win && w->client_win != cw)
		win_unmark_client(ps, w);

	// Mark the new one
	win_mark_client(ps, w, cw);
}

/**
 * Free all resources in a <code>struct _win</code>.
 */
void free_win_res(session_t *ps, struct managed_win *w) {
	// No need to call backend release_image here because
	// finish_unmap_win should've done that for us.
	// XXX unless we are called by session_destroy

	pixman_region32_fini(&w->bounding_shape);
	// BadDamage may be thrown if the window is destroyed
	set_ignore_cookie(ps, xcb_damage_destroy(ps->c, w->damage));
	rc_region_unref(&w->reg_ignore);
	free(w->name);
	free(w->class_instance);
	free(w->class_general);
	free(w->role);
}

/// Insert a new window after list_node `prev`
/// New window will be in unmapped state
static struct win *add_win(session_t *ps, xcb_window_t id, struct list_node *prev) {
	log_debug("Adding window %#010x", id);
	struct win *old_w = NULL;
	HASH_FIND_INT(ps->windows, &id, old_w);
	assert(old_w == NULL);

	auto new_w = cmalloc(struct win);
	list_insert_after(prev, &new_w->stack_neighbour);
	new_w->id = id;
	new_w->managed = false;
	new_w->is_new = true;
	new_w->destroyed = false;

	HASH_ADD_INT(ps->windows, id, new_w);
	ps->pending_updates = true;
	return new_w;
}

/// Insert a new win entry at the top of the stack
struct win *add_win_top(session_t *ps, xcb_window_t id) {
	return add_win(ps, id, &ps->window_stack);
}

/// Insert a new window above window with id `below`, if there is no window, add to top
/// New window will be in unmapped state
struct win *add_win_above(session_t *ps, xcb_window_t id, xcb_window_t below) {
	struct win *w = NULL;
	HASH_FIND_INT(ps->windows, &below, w);
	if (!w) {
		if (!list_is_empty(&ps->window_stack)) {
			// `below` window is not found even if the window stack is not
			// empty
			return NULL;
		}
		return add_win_top(ps, id);
	} else {
		// we found something from the hash table, so if the stack is empty,
		// we are in an inconsistent state.
		assert(!list_is_empty(&ps->window_stack));
		return add_win(ps, id, w->stack_neighbour.prev);
	}
}

/// Query the Xorg for information about window `win`
/// `win` pointer might become invalid after this function returns
/// Returns the pointer to the window, might be different from `w`
struct win *fill_win(session_t *ps, struct win *w) {
	static const struct managed_win win_def = {
	    // No need to initialize. (or, you can think that
	    // they are initialized right here).
	    // The following ones are updated during paint or paint preprocess
	    .shadow_opacity = 0.0,
	    .to_paint = false,
	    .frame_opacity = 1.0,
	    .dim = false,
	    .invert_color = false,
	    .blur_background = false,
	    .reg_ignore = NULL,
	    // The following ones are updated for other reasons
	    .pixmap_damaged = false,          // updated by damage events
	    .state = WSTATE_UNMAPPED,         // updated by window state changes
	    .in_openclose = true,             // set to false after first map is done,
	                                      // true here because window is just created
	    .reg_ignore_valid = false,        // set to true when damaged
	    .flags = WIN_FLAGS_IMAGES_NONE,   // updated by property/attributes/etc change

	    // Runtime variables, updated by dbus
	    .fade_force = UNSET,
	    .shadow_force = UNSET,
	    .focused_force = UNSET,
	    .invert_color_force = UNSET,

	    // Initialized in this function
	    .a = {0},
	    .pictfmt = NULL,
	    .client_pictfmt = NULL,
	    .widthb = 0,
	    .heightb = 0,
	    .shadow_dx = 0,
	    .shadow_dy = 0,
	    .shadow_width = 0,
	    .shadow_height = 0,
	    .damage = XCB_NONE,

	    // Not initialized until mapped, this variables
	    // have no meaning or have no use until the window
	    // is mapped
	    .win_image = NULL,
	    .shadow_image = NULL,
	    .prev_trans = NULL,
	    .shadow = false,
	    .xinerama_scr = -1,
	    .mode = WMODE_TRANS,
	    .ever_damaged = false,
	    .client_win = XCB_NONE,
	    .leader = XCB_NONE,
	    .cache_leader = XCB_NONE,
	    .window_type = WINTYPE_UNKNOWN,
	    .wmwin = false,
	    .focused = false,
	    .opacity = 0,
	    .opacity_target = 0,
	    .has_opacity_prop = false,
	    .opacity_prop = OPAQUE,
	    .opacity_is_set = false,
	    .opacity_set = 1,
	    .frame_extents = MARGIN_INIT,        // in win_mark_client
	    .bounding_shaped = false,
	    .bounding_shape = {0},
	    .rounded_corners = false,
	    .paint_excluded = false,
	    .unredir_if_possible_excluded = false,
	    .prop_shadow = -1,
	    // following 4 are set in win_mark_client
	    .name = NULL,
	    .class_instance = NULL,
	    .class_general = NULL,
	    .role = NULL,
	};

	assert(!w->destroyed);
	assert(w->is_new);

	w->is_new = false;

	// Reject overlay window and already added windows
	if (w->id == ps->overlay) {
		return w;
	}

	auto duplicated_win = find_managed_win(ps, w->id);
	if (duplicated_win) {
		log_debug("Window %#010x (recorded name: %s) added multiple times", w->id,
		          duplicated_win->name);
		return &duplicated_win->base;
	}

	log_debug("Managing window %#010x", w->id);
	xcb_get_window_attributes_cookie_t acookie = xcb_get_window_attributes(ps->c, w->id);
	xcb_get_window_attributes_reply_t *a =
	    xcb_get_window_attributes_reply(ps->c, acookie, NULL);
	if (!a || a->map_state == XCB_MAP_STATE_UNVIEWABLE) {
		// Failed to get window attributes or geometry probably means
		// the window is gone already. Unviewable means the window is
		// already reparented elsewhere.
		// BTW, we don't care about Input Only windows, except for stacking
		// proposes, so we need to keep track of them still.
		free(a);
		return w;
	}

	if (a->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		// No need to manage this window, but we still keep it on the window stack
		w->managed = false;
		free(a);
		return w;
	}

	// Allocate and initialize the new win structure
	auto new_internal = cmalloc(struct managed_win_internal);
	auto new = (struct managed_win *)new_internal;
	new_internal->pending_updates = 0;

	// Fill structure
	// We only need to initialize the part that are not initialized
	// by map_win
	*new = win_def;
	new->base = *w;
	new->base.managed = true;
	new->a = *a;
	pixman_region32_init(&new->bounding_shape);

	free(a);

	// Create Damage for window (if not Input Only)
	new->damage = x_new_id(ps->c);
	xcb_generic_error_t *e = xcb_request_check(
	    ps->c, xcb_damage_create_checked(ps->c, new->damage, w->id,
	                                     XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
	if (e) {
		free(e);
		free(new);
		return w;
	}

	new->pictfmt = x_get_pictform_for_visual(ps->c, new->a.visual);
	new->client_pictfmt = NULL;

	list_replace(&w->stack_neighbour, &new->base.stack_neighbour);
	struct win *replaced = NULL;
	HASH_REPLACE_INT(ps->windows, id, &new->base, replaced);
	assert(replaced == w);
	free(w);

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_added(ps, &new->base);
	}
#endif
	return &new->base;
}

/**
 * Set leader of a window.
 */
static inline void win_set_leader(session_t *ps, struct managed_win *w, xcb_window_t nleader) {
	// If the leader changes
	if (w->leader != nleader) {
		xcb_window_t cache_leader_old = win_get_leader(ps, w);

		w->leader = nleader;

		// Forcefully do this to deal with the case when a child window
		// gets mapped before parent, or when the window is a waypoint
		clear_cache_win_leaders(ps);

		// Update the old and new window group and active_leader if the window
		// could affect their state.
		xcb_window_t cache_leader = win_get_leader(ps, w);
		if (win_is_focused_real(ps, w) && cache_leader_old != cache_leader) {
			ps->active_leader = cache_leader;

			group_update_focused(ps, cache_leader_old);
			group_update_focused(ps, cache_leader);
		}
		// Otherwise, at most the window itself is affected
		else {
			win_update_focused(ps, w);
		}

		// Update everything related to conditions
		win_on_factor_change(ps, w);
	}
}

/**
 * Update leader of a window.
 */
void win_update_leader(session_t *ps, struct managed_win *w) {
	xcb_window_t leader = XCB_NONE;

	// Read the leader properties
	if (ps->o.detect_transient && !leader)
		leader = wid_get_prop_window(ps, w->client_win, ps->atoms->aWM_TRANSIENT_FOR);

	if (ps->o.detect_client_leader && !leader)
		leader = wid_get_prop_window(ps, w->client_win, ps->atoms->aWM_CLIENT_LEADER);

	win_set_leader(ps, w, leader);

	log_trace("(%#010x): client %#010x, leader %#010x, cache %#010x", w->base.id,
	          w->client_win, w->leader, win_get_leader(ps, w));
}

/**
 * Internal function of win_get_leader().
 */
static xcb_window_t win_get_leader_raw(session_t *ps, struct managed_win *w, int recursions) {
	// Rebuild the cache if needed
	if (!w->cache_leader && (w->client_win || w->leader)) {
		// Leader defaults to client window
		if (!(w->cache_leader = w->leader))
			w->cache_leader = w->client_win;

		// If the leader of this window isn't itself, look for its ancestors
		if (w->cache_leader && w->cache_leader != w->client_win) {
			auto wp = find_toplevel(ps, w->cache_leader);
			if (wp) {
				// Dead loop?
				if (recursions > WIN_GET_LEADER_MAX_RECURSION)
					return XCB_NONE;

				w->cache_leader = win_get_leader_raw(ps, wp, recursions + 1);
			}
		}
	}

	return w->cache_leader;
}

/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_get_class(session_t *ps, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	// Can't do anything if there's no client window
	if (!w->client_win)
		return false;

	// Free and reset old strings
	free(w->class_instance);
	free(w->class_general);
	w->class_instance = NULL;
	w->class_general = NULL;

	// Retrieve the property string list
	if (!wid_get_text_prop(ps, w->client_win, ps->atoms->aWM_CLASS, &strlst, &nstr))
		return false;

	// Copy the strings if successful
	w->class_instance = strdup(strlst[0]);

	if (nstr > 1)
		w->class_general = strdup(strlst[1]);

	XFreeStringList(strlst);

	log_trace("(%#010x): client = %#010x, "
	          "instance = \"%s\", general = \"%s\"",
	          w->base.id, w->client_win, w->class_instance, w->class_general);

	return true;
}

/**
 * Handle window focus change.
 */
static void win_on_focus_change(session_t *ps, struct managed_win *w) {
	// If window grouping detection is enabled
	if (ps->o.track_leader) {
		xcb_window_t leader = win_get_leader(ps, w);

		// If the window gets focused, replace the old active_leader
		if (win_is_focused_real(ps, w) && leader != ps->active_leader) {
			xcb_window_t active_leader_old = ps->active_leader;

			ps->active_leader = leader;

			group_update_focused(ps, active_leader_old);
			group_update_focused(ps, leader);
		}
		// If the group get unfocused, remove it from active_leader
		else if (!win_is_focused_real(ps, w) && leader &&
		         leader == ps->active_leader && !group_is_focused(ps, leader)) {
			ps->active_leader = XCB_NONE;
			group_update_focused(ps, leader);
		}

		// The window itself must be updated anyway
		win_update_focused(ps, w);
	}
	// Otherwise, only update the window itself
	else {
		win_update_focused(ps, w);
	}

	// Update everything related to conditions
	win_on_factor_change(ps, w);

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		if (win_is_focused_real(ps, w))
			cdbus_ev_win_focusin(ps, &w->base);
		else
			cdbus_ev_win_focusout(ps, &w->base);
	}
#endif
}

/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct managed_win *w) {
	// Unmapped windows will have their focused state reset on map
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	if (win_is_focused_real(ps, w)) {
		return;
	}

	auto old_active_win = ps->active_win;
	ps->active_win = w;
	assert(win_is_focused_real(ps, w));

	if (old_active_win) {
		win_on_focus_change(ps, old_active_win);
	}
	win_on_focus_change(ps, w);
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(const struct managed_win *w, region_t *res) {
	pixman_region32_clear(res);
	pixman_region32_union_rect(res, res, w->g.x, w->g.y, (uint)w->widthb, (uint)w->heightb);

	if (w->shadow) {
		assert(w->shadow_width >= 0 && w->shadow_height >= 0);
		pixman_region32_union_rect(res, res, w->g.x + w->shadow_dx,
		                           w->g.y + w->shadow_dy, (uint)w->shadow_width,
		                           (uint)w->shadow_height);
	}
}

gen_by_val(win_extents);

/**
 * Update the out-dated bounding shape of a window.
 *
 * Mark the window shape as updated
 */
void win_update_bounding_shape(session_t *ps, struct managed_win *w) {
	if (ps->shape_exists)
		w->bounding_shaped = win_bounding_shaped(ps, w->base.id);

	pixman_region32_clear(&w->bounding_shape);
	// Start with the window rectangular region
	win_get_region_local(w, &w->bounding_shape);

	// Only request for a bounding region if the window is shaped
	// (while loop is used to avoid goto, not an actual loop)
	while (w->bounding_shaped) {
		/*
		 * if window doesn't exist anymore,  this will generate an error
		 * as well as not generate a region.
		 */

		xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(
		    ps->c,
		    xcb_shape_get_rectangles(ps->c, w->base.id, XCB_SHAPE_SK_BOUNDING), NULL);

		if (!r)
			break;

		xcb_rectangle_t *xrects = xcb_shape_get_rectangles_rectangles(r);
		int nrects = xcb_shape_get_rectangles_rectangles_length(r);
		rect_t *rects = from_x_rects(nrects, xrects);
		free(r);

		region_t br;
		pixman_region32_init_rects(&br, rects, nrects);
		free(rects);

		// Add border width because we are using a different origin.
		// X thinks the top left of the inner window is the origin
		// (for the bounding shape, althought xcb_get_geometry thinks
		//  the outer top left (outer means outside of the window
		//  border) is the origin),
		// We think the top left of the border is the origin
		pixman_region32_translate(&br, w->g.border_width, w->g.border_width);

		// Intersect the bounding region we got with the window rectangle, to
		// make sure the bounding region is not bigger than the window
		// rectangle
		pixman_region32_intersect(&w->bounding_shape, &w->bounding_shape, &br);
		pixman_region32_fini(&br);
		break;
	}

	if (w->bounding_shaped && ps->o.detect_rounded_corners) {
		w->rounded_corners = win_has_rounded_corners(w);
	}

	// Window shape changed, we should free old wpaint and shadow pict
	// log_trace("free out dated pict");
	if (w->state != WSTATE_UNMAPPED) {
		// Note we only do this when screen is redirected, because
		// otherwise win_data is not valid
		assert(w->state != WSTATE_UNMAPPING && w->state != WSTATE_DESTROYING);
		w->flags |= WIN_FLAGS_IMAGES_STALE;
		ps->pending_updates = true;
	}

	win_on_factor_change(ps, w);
}

/**
 * Reread opacity property of a window.
 */
void win_update_opacity_prop(session_t *ps, struct managed_win *w) {
	// get frame opacity first
	w->has_opacity_prop = wid_get_opacity_prop(ps, w->base.id, OPAQUE, &w->opacity_prop);

	if (w->has_opacity_prop)
		// opacity found
		return;

	if (ps->o.detect_client_opacity && w->client_win && w->base.id == w->client_win)
		// checking client opacity not allowed
		return;

	// get client opacity
	w->has_opacity_prop =
	    wid_get_opacity_prop(ps, w->client_win, OPAQUE, &w->opacity_prop);
}

/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(session_t *ps, struct managed_win *w, xcb_window_t client) {
	winprop_t prop = wid_get_prop(ps, client, ps->atoms->a_NET_FRAME_EXTENTS, 4L,
	                              XCB_ATOM_CARDINAL, 32);

	if (prop.nitems == 4) {
		const int32_t extents[4] = {
		    to_int_checked(prop.c32[0]),
		    to_int_checked(prop.c32[1]),
		    to_int_checked(prop.c32[2]),
		    to_int_checked(prop.c32[3]),
		};
		const bool changed = w->frame_extents.left != extents[0] ||
		                     w->frame_extents.right != extents[1] ||
		                     w->frame_extents.top != extents[2] ||
		                     w->frame_extents.bottom != extents[3];
		w->frame_extents.left = extents[0];
		w->frame_extents.right = extents[1];
		w->frame_extents.top = extents[2];
		w->frame_extents.bottom = extents[3];

		// If frame_opacity != 1, then frame of this window
		// is not included in reg_ignore of underneath windows
		if (ps->o.frame_opacity == 1 && changed)
			w->reg_ignore_valid = false;
	}

	log_trace("(%#010x): %d, %d, %d, %d", w->base.id, w->frame_extents.left,
	          w->frame_extents.right, w->frame_extents.top, w->frame_extents.bottom);

	free_winprop(&prop);
}

bool win_is_region_ignore_valid(session_t *ps, const struct managed_win *w) {
	win_stack_foreach_managed(i, &ps->window_stack) {
		if (i == w)
			break;
		if (!i->reg_ignore_valid)
			return false;
	}
	return true;
}

/**
 * Stop listening for events on a particular window.
 */
void win_ev_stop(session_t *ps, const struct win *w) {
	xcb_change_window_attributes(ps->c, w->id, XCB_CW_EVENT_MASK, (const uint32_t[]){0});

	if (!w->managed) {
		return;
	}

	auto mw = (struct managed_win *)w;
	if (mw->client_win) {
		xcb_change_window_attributes(ps->c, mw->client_win, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){0});
	}

	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c, w->id, 0);
	}
}

/// Finish the unmapping of a window (e.g. after fading has finished).
/// Doesn't free `w`
static void unmap_win_finish(session_t *ps, struct managed_win *w) {
	w->ever_damaged = false;
	w->reg_ignore_valid = false;
	w->state = WSTATE_UNMAPPED;

	// We are in unmap_win, this window definitely was viewable
	if (ps->backend_data) {
		win_release_images(ps->backend_data, w);
	} else {
		assert(!w->win_image);
		assert(!w->shadow_image);
	}

	// Try again at binding images when the window is mapped next time
	w->flags &= ~WIN_FLAGS_IMAGE_ERROR;
}

/// Finish the destruction of a window (e.g. after fading has finished).
/// Frees `w`
static void destroy_win_finish(session_t *ps, struct win *w) {
	log_trace("Trying to finish destroying (%#010x)", w->id);

	auto next_w = win_stack_find_next_managed(ps, &w->stack_neighbour);
	list_remove(&w->stack_neighbour);

	if (w->managed) {
		auto mw = (struct managed_win *)w;

		if (mw->state != WSTATE_UNMAPPED) {
			// Only UNMAPPED state has window resources freed, otherwise
			// we need to call unmap_win_finish to free them.
			// XXX actually we unmap_win_finish only frees the rendering
			//     resources, we still need to call free_win_res. will fix
			//     later.
			unmap_win_finish(ps, mw);
		}

		// Invalidate reg_ignore of windows below this one
		// TODO what if next_w is not mapped??
		// TODO seriously figure out how reg_ignore behaves.
		//      I think if `w` is unmapped, and destroyed after
		//      paint happened at least once, w->reg_ignore_valid would
		//      be true, and there is no need to invalid w->next->reg_ignore
		//      when w is destroyed.
		if (next_w) {
			rc_region_unref(&next_w->reg_ignore);
			next_w->reg_ignore_valid = false;
		}

		if (mw == ps->active_win) {
			// Usually, the window cannot be the focused at destruction.
			// FocusOut should be generated before the window is destroyed. We
			// do this check just to be completely sure we don't have dangling
			// references.
			log_debug("window %#010x (%s) is destroyed while being focused",
			          w->id, mw->name);
			ps->active_win = NULL;
		}

		free_win_res(ps, mw);

		// Drop w from all prev_trans to avoid accessing freed memory in
		// repair_win()
		// TODO there can only be one prev_trans pointing to w
		win_stack_foreach_managed(w2, &ps->window_stack) {
			if (mw == w2->prev_trans) {
				w2->prev_trans = NULL;
			}
		}
	}

	free(w);
}

static void map_win_finish(struct managed_win *w) {
	w->in_openclose = false;
	w->state = WSTATE_MAPPED;
}

/// Move window `w` so it's before `next` in the list
static inline void restack_win(session_t *ps, struct win *w, struct list_node *next) {
	struct managed_win *mw = NULL;
	if (w->managed) {
		mw = (struct managed_win *)w;
	}

	if (mw) {
		// This invalidates all reg_ignore below the new stack position of `w`
		mw->reg_ignore_valid = false;
		rc_region_unref(&mw->reg_ignore);

		// This invalidates all reg_ignore below the old stack position of `w`
		auto next_w = win_stack_find_next_managed(ps, &w->stack_neighbour);
		if (next_w) {
			next_w->reg_ignore_valid = false;
			rc_region_unref(&next_w->reg_ignore);
		}
	}

	list_move_before(&w->stack_neighbour, next);

	// add damage for this window
	if (mw) {
		add_damage_from_win(ps, mw);
	}

#ifdef DEBUG_RESTACK
	log_trace("Window stack modified. Current stack:");
	for (auto c = ps->list; c; c = c->next) {
		const char *desc = "";
		if (c->state == WSTATE_DESTROYING) {
			desc = "(D) ";
		}
		log_trace("%#010x \"%s\" %s", c->id, c->name, desc);
	}
#endif
}

/// Move window `w` so it's right above `below`
void restack_above(session_t *ps, struct win *w, xcb_window_t below) {
	xcb_window_t old_below;

	if (!list_node_is_last(&ps->window_stack, &w->stack_neighbour)) {
		old_below = list_next_entry(w, stack_neighbour)->id;
	} else {
		old_below = XCB_NONE;
	}
	log_debug("Restack %#010x (%s), old_below: %#010x, new_below: %#010x", w->id,
	          win_get_name_if_managed(w), old_below, below);

	if (old_below != below) {
		struct list_node *new_next;
		if (!below) {
			new_next = &ps->window_stack;
		} else {
			struct win *tmp_w = NULL;
			HASH_FIND_INT(ps->windows, &below, tmp_w);

			if (!tmp_w) {
				log_error("Failed to found new below window %#010x.", below);
				return;
			}

			new_next = &tmp_w->stack_neighbour;
		}
		restack_win(ps, w, new_next);
	}
}

void restack_bottom(session_t *ps, struct win *w) {
	restack_above(ps, w, 0);
}

void restack_top(session_t *ps, struct win *w) {
	log_debug("Restack %#010x (%s) to top", w->id, win_get_name_if_managed(w));
	if (&w->stack_neighbour == ps->window_stack.next) {
		// already at top
		return;
	}
	restack_win(ps, w, ps->window_stack.next);
}

/// Start destroying a window. Windows cannot always be destroyed immediately
/// because of fading and such.
///
/// @return whether the window has finished destroying and is freed
bool destroy_win_start(session_t *ps, struct win *w) {
	auto mw = (struct managed_win *)w;
	assert(w);

	log_debug("Destroying %#010x \"%s\", managed = %d", w->id,
	          (w->managed ? mw->name : NULL), w->managed);

	// Delete destroyed window from the hash table, even though the window might still
	// be rendered for a while. We need to make sure future window with the same
	// window id won't confuse us. Keep the window in the window stack if it's managed
	// and mapped, since we might still need to render it (e.g. fading out). Window
	// will be removed from the stack when it finishes destroying.
	HASH_DEL(ps->windows, w);

	if (!w->managed || mw->state == WSTATE_UNMAPPED) {
		// Window is already unmapped, or is an unmanged window, just destroy it
		destroy_win_finish(ps, w);
		return true;
	}

	if (w->managed) {
		// Update state flags of a managed window
		mw->state = WSTATE_DESTROYING;
		mw->a.map_state = XCB_MAP_STATE_UNMAPPED;
		mw->in_openclose = true;

		// Clear PIXMAP_STALE flag, since the window is destroyed there is no
		// pixmap available so STALE doesn't make sense.
		mw->flags &= ~WIN_FLAGS_PIXMAP_STALE;
	}

	// don't need win_ev_stop because the window is gone anyway
#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_destroyed(ps, w);
	}
#endif

	if (!ps->redirected) {
		// Skip transition if we are not rendering
		return win_skip_fading(ps, mw);
	}

	return false;
}

void unmap_win_start(session_t *ps, struct managed_win *w) {
	auto internal_w = (struct managed_win_internal *)w;
	assert(w);
	assert(w->base.managed);
	assert(w->a._class != XCB_WINDOW_CLASS_INPUT_ONLY);

	log_debug("Unmapping %#010x \"%s\"", w->base.id, w->name);

	if (unlikely(w->state == WSTATE_DESTROYING)) {
		log_warn("Trying to undestroy a window?");
		assert(false);
	}

	if (unlikely(w->state == WSTATE_UNMAPPING || w->state == WSTATE_UNMAPPED)) {
		if (internal_w->pending_updates & WIN_UPDATE_MAP) {
			internal_w->pending_updates &= ~(unsigned long)WIN_UPDATE_MAP;
		} else {
			log_warn("Trying to unmapping an already unmapped window %#010x "
			         "\"%s\"",
			         w->base.id, w->name);
			assert(false);
		}
		return;
	}

	// Note we don't update focused window here. This will either be
	// triggered by subsequence Focus{In, Out} event, or by recheck_focus

	w->a.map_state = XCB_MAP_STATE_UNMAPPED;
	w->state = WSTATE_UNMAPPING;
	w->opacity_target = win_calc_opacity_target(ps, w, false);

	// Clear PIXMAP_STALE flag, since the window is unmapped there is no pixmap
	// available so STALE doesn't make sense.
	w->flags &= ~WIN_FLAGS_PIXMAP_STALE;

	// don't care about properties anymore
	win_ev_stop(ps, &w->base);

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_unmapped(ps, &w->base);
	}
#endif

	if (!ps->redirected) {
		CHECK(!win_skip_fading(ps, w));
	}
}

/**
 * Execute fade callback of a window if fading finished.
 *
 * @return whether the window is destroyed and freed
 */
bool win_check_fade_finished(session_t *ps, struct managed_win *w) {
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		// No fading in progress
		assert(w->opacity_target == w->opacity);
		return false;
	}
	if (w->opacity == w->opacity_target) {
		switch (w->state) {
		case WSTATE_UNMAPPING: unmap_win_finish(ps, w); return false;
		case WSTATE_DESTROYING: destroy_win_finish(ps, &w->base); return true;
		case WSTATE_MAPPING: map_win_finish(w); return false;
		case WSTATE_FADING: w->state = WSTATE_MAPPED; break;
		default: unreachable;
		}
	}

	return false;
}

/// Skip the current in progress fading of window,
/// transition the window straight to its end state
///
/// @return whether the window is destroyed and freed
bool win_skip_fading(session_t *ps, struct managed_win *w) {
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		assert(w->opacity_target == w->opacity);
		return false;
	}
	log_debug("Skipping fading process of window %#010x (%s)", w->base.id, w->name);
	w->opacity = w->opacity_target;
	return win_check_fade_finished(ps, w);
}

/**
 * Get the Xinerama screen a window is on.
 *
 * Return an index >= 0, or -1 if not found.
 *
 * TODO move to x.c
 * TODO use xrandr
 */
void win_update_screen(session_t *ps, struct managed_win *w) {
	w->xinerama_scr = -1;

	for (int i = 0; i < ps->xinerama_nscrs; i++) {
		auto e = pixman_region32_extents(&ps->xinerama_scr_regs[i]);
		if (e->x1 <= w->g.x && e->y1 <= w->g.y && e->x2 >= w->g.x + w->widthb &&
		    e->y2 >= w->g.y + w->heightb) {
			w->xinerama_scr = i;
			return;
		}
	}
}

/// Map an already registered window
void map_win_start(session_t *ps, struct managed_win *w) {
	assert(ps->server_grabbed);
	assert(w);

	// Don't care about window mapping if it's an InputOnly window
	// Also, try avoiding mapping a window twice
	if (w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		return;
	}

	log_debug("Mapping (%#010x \"%s\")", w->base.id, w->name);

	assert(w->state != WSTATE_DESTROYING);
	if (w->state != WSTATE_UNMAPPED && w->state != WSTATE_UNMAPPING) {
		log_warn("Mapping an already mapped window");
		return;
	}

	if (w->state == WSTATE_UNMAPPING) {
		CHECK(!win_skip_fading(ps, w));
		// We skipped the unmapping process, the window was rendered, now it is
		// not anymore. So we need to mark the then unmapping window as damaged.
		//
		// Solves problem when, for example, a window is unmapped then mapped in a
		// different location
		add_damage_from_win(ps, w);
		assert(w);
	}

	assert(w->state == WSTATE_UNMAPPED);
	assert((w->flags & WIN_FLAGS_IMAGES_NONE) == WIN_FLAGS_IMAGES_NONE);

	// We stopped processing window size change when we were unmapped, refresh the
	// size of the window
	xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(ps->c, w->base.id);
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(ps->c, gcookie, NULL);

	if (!g) {
		log_error("Failed to get the geometry of window %#010x", w->base.id);
		return;
	}

	w->g = *g;
	free(g);

	win_on_win_size_change(ps, w);
	log_trace("Window size: %dx%d", w->g.width, w->g.height);

	// Rant: window size could change after we queried its geometry here and before
	// we get its pixmap. Later, when we get back to the event processing loop, we
	// will get the notification about size change from Xserver and try to refresh the
	// pixmap, while the pixmap is actually already up-to-date (i.e. the notification
	// is stale). There is basically no real way to prevent this, aside from grabbing
	// the server.

	// XXX Can we assume map_state is always viewable?
	w->a.map_state = XCB_MAP_STATE_VIEWABLE;

	win_update_screen(ps, w);

	// Set window event mask before reading properties so that no property
	// changes are lost
	xcb_change_window_attributes(
	    ps->c, w->base.id, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, w->base.id, WIN_EVMODE_FRAME)});

	// Notify compton when the shape of a window changes
	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c, w->base.id, 1);
	}

	// Update window mode here to check for ARGB windows
	w->mode = win_calc_mode(w);

	// Detect client window here instead of in add_win() as the client
	// window should have been prepared at this point
	if (!w->client_win) {
		win_recheck_client(ps, w);
	} else {
		// Re-mark client window here
		win_mark_client(ps, w, w->client_win);
	}
	assert(w->client_win);

	log_debug("Window (%#010x) has type %s", w->base.id, WINTYPES[w->window_type]);

	// Update window focus state
	win_update_focused(ps, w);

	// Update opacity and dim state
	win_update_opacity_prop(ps, w);

	// Check for _COMPTON_SHADOW
	if (ps->o.respect_prop_shadow) {
		win_update_prop_shadow_raw(ps, w);
	}

	// Many things above could affect shadow
	win_determine_shadow(ps, w);

	// XXX We need to make sure that win_data is available
	// iff `state` is MAPPED
	w->state = WSTATE_MAPPING;
	w->opacity_target = win_calc_opacity_target(ps, w, false);

	log_debug("Window %#010x has opacity %f, opacity target is %f", w->base.id,
	          w->opacity, w->opacity_target);

	win_determine_blur_background(ps, w);

	// Cannot set w->ever_damaged = false here, since window mapping could be
	// delayed, so a damage event might have already arrived before this function
	// is called. But this should be unnecessary in the first place, since
	// ever_damaged is set to false in unmap_win_finish anyway.

	// We stopped listening on ShapeNotify events
	// when the window is unmapped (XXX we shouldn't),
	// so the shape of the window might have changed,
	// update. (Issue #35)
	//
	// Also this sets the WIN_FLAGS_IMAGES_STALE flag so later in the critical section
	// the window's image will be bound
	win_update_bounding_shape(ps, w);

	assert((w->flags & WIN_FLAGS_IMAGES_STALE) == WIN_FLAGS_IMAGES_STALE);

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_mapped(ps, &w->base);
	}
#endif

	if (!ps->redirected) {
		CHECK(!win_skip_fading(ps, w));
	}
}

/**
 * Find a managed window from window id in window linked list of the session.
 */
struct win *find_win(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	struct win *w = NULL;
	HASH_FIND_INT(ps->windows, &id, w);
	assert(w == NULL || !w->destroyed);
	return w;
}

/**
 * Find a managed window from window id in window linked list of the session.
 */
struct managed_win *find_managed_win(session_t *ps, xcb_window_t id) {
	struct win *w = find_win(ps, id);
	if (!w || !w->managed) {
		return NULL;
	}

	auto mw = (struct managed_win *)w;
	assert(mw->state != WSTATE_DESTROYING);
	return mw;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct win object of the found window, NULL if not found
 */
struct managed_win *find_toplevel(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}

		auto mw = (struct managed_win *)w;
		if (mw->client_win == id) {
			return mw;
		}
	}

	return NULL;
}

/**
 * Find out the WM frame of a client window by querying X.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
struct managed_win *find_toplevel2(session_t *ps, xcb_window_t wid) {
	// TODO this should probably be an "update tree", then find_toplevel.
	//      current approach is a bit more "racy"
	struct win *w = NULL;

	// We traverse through its ancestors to find out the frame
	// Using find_win here because if we found a unmanaged window we know about, we
	// can stop early.
	while (wid && wid != ps->root && !(w = find_win(ps, wid))) {
		// xcb_query_tree probably fails if you run compton when X is somehow
		// initializing (like add it in .xinitrc). In this case
		// just leave it alone.
		auto reply = xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, wid), NULL);
		if (reply == NULL) {
			break;
		}

		wid = reply->parent;
		free(reply);
	}

	if (w == NULL || !w->managed) {
		return NULL;
	}

	return (struct managed_win *)w;
}

/**
 * Check if a rectangle includes the whole screen.
 */
static inline bool rect_is_fullscreen(const session_t *ps, int x, int y, int wid, int hei) {
	return (x <= 0 && y <= 0 && (x + wid) >= ps->root_width && (y + hei) >= ps->root_height);
}

/**
 * Check if a window is fulscreen using EWMH
 */
static inline bool
win_is_fullscreen_xcb(xcb_connection_t *c, const struct atom *a, const xcb_window_t w) {
	xcb_get_property_cookie_t prop =
	    xcb_get_property(c, 0, w, a->a_NET_WM_STATE, XCB_ATOM_ATOM, 0, 12);
	xcb_get_property_reply_t *reply = xcb_get_property_reply(c, prop, NULL);
	if (!reply)
		return false;

	if (reply->length) {
		xcb_atom_t *val = xcb_get_property_value(reply);
		for (uint32_t i = 0; i < reply->length; i++) {
			if (val[i] != a->a_NET_WM_STATE_FULLSCREEN)
				continue;
			free(reply);
			return true;
		}
	}
	free(reply);
	return false;
}

/// Queue an update on a window. A series of sanity checks are performed
void win_queue_update(struct managed_win *_w, enum win_update update) {
	auto w = (struct managed_win_internal *)_w;
	assert(popcount(update) == 1);

	if (unlikely(_w->state == WSTATE_DESTROYING)) {
		log_error("Updates queued on a destroyed window %#010x (%s)", _w->base.id,
		          _w->name);
		return;
	}

	w->pending_updates |= WIN_UPDATE_MAP;
}

/// Process pending updates on a window. Has to be called in X critical section
void win_process_updates(struct session *ps, struct managed_win *_w) {
	assert(ps->server_grabbed);
	auto w = (struct managed_win_internal *)_w;

	if (w->pending_updates & WIN_UPDATE_MAP) {
		map_win_start(ps, _w);
	}

	w->pending_updates = 0;
}

/**
 * Check if a window is a fullscreen window.
 *
 * It's not using w->border_size for performance measures.
 */
bool win_is_fullscreen(const session_t *ps, const struct managed_win *w) {
	if (!ps->o.no_ewmh_fullscreen && win_is_fullscreen_xcb(ps->c, ps->atoms, w->client_win))
		return true;
	return rect_is_fullscreen(ps, w->g.x, w->g.y, w->widthb, w->heightb) &&
	       (!w->bounding_shaped || w->rounded_corners);
}

/**
 * Check if a window is really focused.
 */
bool win_is_focused_real(const session_t *ps, const struct managed_win *w) {
	return w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->active_win == w;
}

// Find the managed window immediately below `i` in the window stack
struct managed_win *
win_stack_find_next_managed(const session_t *ps, const struct list_node *i) {
	while (!list_node_is_last(&ps->window_stack, i)) {
		auto next = list_entry(i->next, struct win, stack_neighbour);
		if (next->managed) {
			return (struct managed_win *)next;
		}
		i = &next->stack_neighbour;
	}
	return NULL;
}

/// Return whether this window is mapped on the X server side
bool win_is_mapped_in_x(const struct managed_win *w) {
	auto iw = (const struct managed_win_internal *)w;
	return w->state == WSTATE_MAPPING || w->state == WSTATE_FADING ||
	       w->state == WSTATE_MAPPED || (iw->pending_updates & WIN_UPDATE_MAP);
}
