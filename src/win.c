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

#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "compton.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "render.h"
#include "string_utils.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "x.h"

#ifdef CONFIG_DBUS
#include "dbus.h"
#endif

#ifdef CONFIG_OPENGL
// TODO remove this include
#include "opengl.h"
#endif

#include "win.h"

#define OPAQUE 0xffffffff

/// Generate a "return by value" function, from a function that returns the
/// region via a region_t pointer argument.
/// Function signature has to be (win *, region_t *)
#define gen_by_val(fun)                                                                  \
	region_t fun##_by_val(const win *w) {                                            \
		region_t ret;                                                            \
		pixman_region32_init(&ret);                                              \
		fun(w, &ret);                                                            \
		return ret;                                                              \
	}

/**
 * Clear leader cache of all windows.
 */
static inline void clear_cache_win_leaders(session_t *ps) {
	WIN_STACK_ITER(ps, w) {
		w->cache_leader = XCB_NONE;
	}
}

static inline void wid_set_opacity_prop(session_t *ps, xcb_window_t wid, opacity_t val) {
	const uint32_t v = val;
	xcb_change_property(ps->c, XCB_PROP_MODE_REPLACE, wid, ps->atom_opacity,
	                    XCB_ATOM_CARDINAL, 32, 1, &v);
}

static inline void wid_rm_opacity_prop(session_t *ps, xcb_window_t wid) {
	xcb_delete_property(ps->c, wid, ps->atom_opacity);
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
		assert(w->state != WSTATE_DESTROYING);
		if (win_get_leader(ps, w) == leader) {
			win_update_focused(ps, w);
		}
	}

	return;
}

/**
 * Return whether a window group is really focused.
 *
 * @param leader leader window ID
 * @return true if the window group is focused, false otherwise
 */
static inline bool group_is_focused(session_t *ps, xcb_window_t leader) {
	if (!leader)
		return false;

	HASH_ITER2(ps->windows, w) {
		assert(w->state != WSTATE_DESTROYING);
		if (win_get_leader(ps, w) == leader && win_is_focused_real(ps, w)) {
			return true;
		}
	}

	return false;
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static void win_get_region_local(const win *w, region_t *res) {
	assert(w->widthb >= 0 && w->heightb >= 0);
	pixman_region32_fini(res);
	pixman_region32_init_rect(res, 0, 0, (uint)w->widthb, (uint)w->heightb);
}

/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
void win_get_region_noframe_local(const win *w, region_t *res) {
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

gen_by_val(win_get_region_noframe_local);

void win_get_region_frame_local(const win *w, region_t *res) {
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
void add_damage_from_win(session_t *ps, win *w) {
	// XXX there was a cached extents region, investigate
	//     if that's better
	region_t extents;
	pixman_region32_init(&extents);
	win_extents(w, &extents);
	add_damage(ps, &extents);
	pixman_region32_fini(&extents);
}

/// Release the images attached to this window
void win_release_image(backend_t *base, win *w) {
	assert(w->win_image || (w->flags & WIN_FLAGS_IMAGE_ERROR));
	if (w->win_image) {
		base->ops->release_image(base, w->win_image);
		w->win_image = NULL;
	}
	if (w->shadow) {
		assert(w->shadow_image || (w->flags & WIN_FLAGS_IMAGE_ERROR));
		if (w->shadow_image) {
			base->ops->release_image(base, w->shadow_image);
			w->shadow_image = NULL;
		}
	}
}

static inline bool
_win_bind_image(session_t *ps, win *w, void **win_image, void **shadow_image) {
	auto pixmap = xcb_generate_id(ps->c);
	auto e = xcb_request_check(
	    ps->c, xcb_composite_name_window_pixmap_checked(ps->c, w->id, pixmap));
	if (e) {
		log_error("Failed to get named pixmap for window %#010x(%s)", w->id, w->name);
		free(e);
		return false;
	}
	log_trace("New named pixmap %#010x", pixmap);
	*win_image = ps->backend_data->ops->bind_pixmap(
	    ps->backend_data, pixmap, x_get_visual_info(ps->c, w->a.visual), true);
	if (!*win_image) {
		return false;
	}
	if (w->shadow) {
		*shadow_image = ps->backend_data->ops->render_shadow(
		    ps->backend_data, w->widthb, w->heightb, ps->gaussian_map, ps->o.shadow_red,
		    ps->o.shadow_green, ps->o.shadow_blue, ps->o.shadow_opacity);
		if (!*shadow_image) {
			log_error("Failed to bind shadow image");
			ps->backend_data->ops->release_image(ps->backend_data, *win_image);
			*win_image = NULL;
			return false;
		}
	}
	return true;
}

bool win_bind_image(session_t *ps, win *w) {
	assert(!w->win_image && !w->shadow_image);
	return _win_bind_image(ps, w, &w->win_image, &w->shadow_image);
}

bool win_try_rebind_image(session_t *ps, win *w) {
	log_trace("Freeing old window image");
	// Must release first, otherwise breaks NVIDIA driver
	win_release_image(ps->backend_data, w);

	return win_bind_image(ps, w);
}

/**
 * Check if a window has rounded corners.
 * XXX This is really dumb
 */
static bool attr_pure win_has_rounded_corners(const win *w) {
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

int win_get_name(session_t *ps, win *w) {
	XTextProperty text_prop = {NULL, XCB_NONE, 0, 0};
	char **strlst = NULL;
	int nstr = 0;

	if (!w->client_win)
		return 0;

	if (!(wid_get_text_prop(ps, w->client_win, ps->atom_name_ewmh, &strlst, &nstr))) {
		log_trace("(%#010x): _NET_WM_NAME unset, falling back to WM_NAME.",
		          w->client_win);

		if (!(XGetWMName(ps->dpy, w->client_win, &text_prop) && text_prop.value)) {
			return -1;
		}
		if (Success != XmbTextPropertyToTextList(ps->dpy, &text_prop, &strlst, &nstr) ||
		    !nstr || !strlst) {
			if (strlst)
				XFreeStringList(strlst);
			cxfree(text_prop.value);
			return -1;
		}
		cxfree(text_prop.value);
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
	          w->id, w->client_win, w->name, ret);
	return ret;
}

int win_get_role(session_t *ps, win *w) {
	char **strlst = NULL;
	int nstr = 0;

	if (!wid_get_text_prop(ps, w->client_win, ps->atom_role, &strlst, &nstr))
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
	          w->id, w->client_win, w->role, ret);
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
	winprop_t prop = wid_get_prop(ps, wid, ps->atom_win_type, 32L, XCB_ATOM_ATOM, 32);

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

	winprop_t prop = wid_get_prop(ps, wid, ps->atom_opacity, 1L, XCB_ATOM_CARDINAL, 32);

	if (prop.nitems) {
		*out = *prop.c32;
		ret = true;
	}

	free_winprop(&prop);

	return ret;
}

// XXX should distinguish between frame has alpha and window body has alpha
bool win_has_alpha(const win *w) {
	return w->pictfmt && w->pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->pictfmt->direct.alpha_mask;
}

winmode_t win_calc_mode(const win *w) {
	if (win_has_alpha(w) || w->opacity < 1.0) {
		return WMODE_TRANS;
	}
	if (w->frame_opacity != 1.0) {
		return WMODE_FRAME_TRANS;
	}
	return WMODE_SOLID;
}

/**
 * Calculate and return the opacity target of a window.
 *
 * If window is inactive and inactive_opacity_override is set, the
 * priority is: (Simulates the old behavior)
 *
 * inactive_opacity > _NET_WM_WINDOW_OPACITY (if not opaque)
 * > window type default opacity
 *
 * Otherwise:
 *
 * _NET_WM_WINDOW_OPACITY (if not opaque)
 * > window type default opacity (if not opaque)
 * > inactive_opacity
 *
 * @param ps current session
 * @param w struct _win object representing the window
 *
 * @return target opacity
 */
double win_calc_opacity_target(session_t *ps, const win *w) {
	double opacity = 1;

	if (w->state == WSTATE_UNMAPPED) {
		// be consistent
		return 0;
	}
	if (w->state == WSTATE_UNMAPPING || w->state == WSTATE_DESTROYING) {
		return 0;
	}
	// Try obeying opacity property and window type opacity firstly
	if (w->has_opacity_prop) {
		opacity = ((double)w->opacity_prop) / OPAQUE;
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
	if (ps->o.inactive_opacity_override && !w->focused)
		opacity = ps->o.inactive_opacity;

	return opacity;
}

/**
 * Determine whether a window is to be dimmed.
 */
bool win_should_dim(session_t *ps, const win *w) {
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
bool win_should_fade(session_t *ps, const win *w) {
	// To prevent it from being overwritten by last-paint value if the window is
	if (w->fade_force != UNSET) {
		return w->fade_force;
	}
	if (ps->o.no_fading_openclose && w->in_openclose) {
		return false;
	}
	if (ps->o.no_fading_destroyed_argb && w->state == WSTATE_DESTROYING &&
	    win_has_alpha(w) && w->client_win && w->client_win != w->id) {
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
void win_update_prop_shadow_raw(session_t *ps, win *w) {
	winprop_t prop =
	    wid_get_prop(ps, w->id, ps->atom_compton_shadow, 1, XCB_ATOM_CARDINAL, 32);

	if (!prop.nitems) {
		w->prop_shadow = -1;
	} else {
		w->prop_shadow = *prop.c32;
	}

	free_winprop(&prop);
}

/**
 * Reread _COMPTON_SHADOW property from a window and update related
 * things.
 */
void win_update_prop_shadow(session_t *ps, win *w) {
	long attr_shadow_old = w->prop_shadow;

	win_update_prop_shadow_raw(ps, w);

	if (w->prop_shadow != attr_shadow_old)
		win_determine_shadow(ps, w);
}

void win_set_shadow(session_t *ps, win *w, bool shadow_new) {
	if (w->shadow == shadow_new) {
		return;
	}

	log_debug("Updating shadow property of window %#010x (%s) to %d", w->id, w->name,
	          shadow_new);
	if (ps->o.experimental_backends && ps->redirected &&
	    w->state != WSTATE_UNMAPPED && !(w->flags & WIN_FLAGS_IMAGE_ERROR)) {
		assert(!w->shadow_image);
		// Create shadow image
		w->shadow_image = ps->backend_data->ops->render_shadow(
		    ps->backend_data, w->widthb, w->heightb, ps->gaussian_map, ps->o.shadow_red,
		    ps->o.shadow_green, ps->o.shadow_blue, ps->o.shadow_opacity);
		if (!w->shadow_image) {
			log_error("Failed to bind shadow image");
			w->shadow_force = OFF;
		}
	}

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
	}
	pixman_region32_fini(&extents);
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
void win_determine_shadow(session_t *ps, win *w) {
	log_debug("Determining shadow of window %#010x (%s)", w->id, w->name);
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

void win_set_invert_color(session_t *ps, win *w, bool invert_color_new) {
	if (w->invert_color == invert_color_new)
		return;

	w->invert_color = invert_color_new;

	add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have color inverted.
 */
void win_determine_invert_color(session_t *ps, win *w) {
	bool invert_color_new = w->invert_color;

	if (UNSET != w->invert_color_force)
		invert_color_new = w->invert_color_force;
	else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE)
		invert_color_new = c2_match(ps, w, ps->o.invert_color_list, NULL);

	win_set_invert_color(ps, w, invert_color_new);
}

void win_set_blur_background(session_t *ps, win *w, bool blur_background_new) {
	if (w->blur_background == blur_background_new)
		return;

	w->blur_background = blur_background_new;

	// Only consider window damaged if it's previously painted with background
	// blurred
	if (!win_is_solid(ps, w) || (ps->o.blur_background_frame && w->frame_opacity != 1))
		add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have background blurred.
 */
void win_determine_blur_background(session_t *ps, win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
		return;

	bool blur_background_new = ps->o.blur_background &&
	                           !c2_match(ps, w, ps->o.blur_background_blacklist, NULL);

	win_set_blur_background(ps, w, blur_background_new);
}

/**
 * Update window opacity according to opacity rules.
 *
 * TODO This override the window's opacity property, may not be
 *      a good idea.
 */
void win_update_opacity_rule(session_t *ps, win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
		return;

	double opacity = 1.0;
	bool is_set = false;
	void *val = NULL;
	if (c2_match(ps, w, ps->o.opacity_rules, &val)) {
		opacity = ((double)(long)val) / 100.0;
		is_set = true;
	}

	if (is_set == w->opacity_is_set && opacity == w->opacity_set)
		return;

	w->opacity_set = opacity;
	w->opacity_is_set = is_set;
	if (!is_set)
		wid_rm_opacity_prop(ps, w->id);
	else
		wid_set_opacity_prop(ps, w->id, (opacity_t)(opacity * OPAQUE));
}

/**
 * Function to be called on window type changes.
 */
void win_on_wtype_change(session_t *ps, win *w) {
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
void win_on_factor_change(session_t *ps, win *w) {
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
void win_on_win_size_change(session_t *ps, win *w) {
	w->widthb = w->g.width + w->g.border_width * 2;
	w->heightb = w->g.height + w->g.border_width * 2;
	w->shadow_dx = ps->o.shadow_offset_x;
	w->shadow_dy = ps->o.shadow_offset_y;
	w->shadow_width = w->widthb + ps->o.shadow_radius * 2;
	w->shadow_height = w->heightb + ps->o.shadow_radius * 2;
	// Invalidate the shadow we built
	if (ps->o.experimental_backends && ps->redirected) {
		if (w->state == WSTATE_MAPPED || w->state == WSTATE_MAPPING ||
		    w->state == WSTATE_FADING) {
			w->flags |= WIN_FLAGS_STALE_IMAGE;
		} else {
			assert(w->state == WSTATE_UNMAPPED);
		}
	} else {
		free_paint(ps, &w->shadow_paint);
	}
}

/**
 * Update window type.
 */
void win_update_wintype(session_t *ps, win *w) {
	const wintype_t wtype_old = w->window_type;

	// Detect window type here
	w->window_type = wid_get_prop_wintype(ps, w->client_win);

	// Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
	// override-redirect windows or windows without WM_TRANSIENT_FOR as
	// _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
	if (WINTYPE_UNKNOWN == w->window_type) {
		if (w->a.override_redirect ||
		    !wid_has_prop(ps, w->client_win, ps->atom_transient))
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
void win_mark_client(session_t *ps, win *w, xcb_window_t client) {
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
		win_get_name(ps, w);
		win_get_class(ps, w);
		win_get_role(ps, w);
	}

	// Update everything related to conditions
	win_on_factor_change(ps, w);

	// Update window focus state
	win_update_focused(ps, w);
}

/**
 * Unmark current client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_unmark_client(session_t *ps, win *w) {
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
void win_recheck_client(session_t *ps, win *w) {
	// Initialize wmwin to false
	w->wmwin = false;

	// Look for the client window

	// Always recursively look for a window with WM_STATE, as Fluxbox
	// sets override-redirect flags on all frame windows.
	xcb_window_t cw = find_client_win(ps, w->id);
	if (cw)
		log_trace("(%#010x): client %#010x", w->id, cw);
	// Set a window's client window to itself if we couldn't find a
	// client window
	if (!cw) {
		cw = w->id;
		w->wmwin = !w->a.override_redirect;
		log_trace("(%#010x): client self (%s)", w->id,
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
void free_win_res(session_t *ps, win *w) {
	// No need to call backend release_image here because
	// finish_unmap_win should've done that for us.
	// XXX unless we are called by session_destroy
	// assert(w->win_data == NULL);
	free_win_res_glx(ps, w);
	free_paint(ps, &w->paint);
	free_paint(ps, &w->shadow_paint);
	// Above should be done during unmapping
	// Except when we are called by session_destroy

	pixman_region32_fini(&w->bounding_shape);
	// BadDamage may be thrown if the window is destroyed
	set_ignore_cookie(ps, xcb_damage_destroy(ps->c, w->damage));
	rc_region_unref(&w->reg_ignore);
	free(w->name);
	free(w->class_instance);
	free(w->class_general);
	free(w->role);
}

// TODO: probably split into win_new (in win.c) and add_win (in compton.c)
void add_win(session_t *ps, xcb_window_t id, xcb_window_t prev) {
	static const win win_def = {
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
	    .queue_configure = {},            // same as above
	    .reg_ignore_valid = false,        // set to true when damaged
	    .flags = 0,                       // updated by property/attributes/etc change

	    // Runtime variables, updated by dbus
	    .fade_force = UNSET,
	    .shadow_force = UNSET,
	    .focused_force = UNSET,
	    .invert_color_force = UNSET,

	    // Initialized in this function
	    .next = NULL,
	    .id = XCB_NONE,
	    .a = {0},
	    .pictfmt = NULL,
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
	    .opacity_tgt = 0,
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

	    // Initialized during paint
	    .paint = PAINT_INIT,
	    .shadow_paint = PAINT_INIT,
	};

	// Reject overlay window and already added windows
	if (id == ps->overlay) {
		return;
	}

	auto duplicated_win = find_win(ps, id);
	if (duplicated_win) {
		log_debug("Window %#010x (recorded name: %s) added multiple times", id,
		          duplicated_win->name);
		return;
	}

	log_debug("Adding window %#010x, prev %#010x", id, prev);
	xcb_get_window_attributes_cookie_t acookie = xcb_get_window_attributes(ps->c, id);
	xcb_get_window_attributes_reply_t *a =
	    xcb_get_window_attributes_reply(ps->c, acookie, NULL);
	if (!a || a->map_state == XCB_MAP_STATE_UNVIEWABLE) {
		// Failed to get window attributes or geometry probably means
		// the window is gone already. Unviewable means the window is
		// already reparented elsewhere.
		// BTW, we don't care about Input Only windows, except for stacking
		// proposes, so we need to keep track of them still.
		free(a);
		return;
	}

	// Allocate and initialize the new win structure
	auto new = cmalloc(win);

	// Fill structure
	// We only need to initialize the part that are not initialized
	// by map_win
	*new = win_def;
	new->id = id;
	new->a = *a;
	pixman_region32_init(&new->bounding_shape);

	free(a);

	// Create Damage for window (if not Input Only)
	if (new->a._class != XCB_WINDOW_CLASS_INPUT_ONLY) {
		new->damage = xcb_generate_id(ps->c);
		xcb_generic_error_t *e = xcb_request_check(
		    ps->c, xcb_damage_create_checked(ps->c, new->damage, id,
		                                     XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
		if (e) {
			free(e);
			free(new);
			return;
		}

		new->pictfmt = x_get_pictform_for_visual(ps->c, new->a.visual);
	}

	// Find window insertion point
	win **p = NULL;
	if (prev) {
		win *w = NULL;
		HASH_FIND_INT(ps->windows, &prev, w);
		assert(w);
		p = w->prev;
	} else {
		p = &ps->window_stack;
	}
	new->next = *p;
	if (new->next) {
		new->next->prev = &new->next;
	}
	new->prev = p;
	*p = new;
	HASH_ADD_INT(ps->windows, id, new);

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_added(ps, new);
	}
#endif
	return;
}

/**
 * Update focused state of a window.
 */
void win_update_focused(session_t *ps, win *w) {
	if (UNSET != w->focused_force) {
		w->focused = w->focused_force;
	} else {
		w->focused = win_is_focused_real(ps, w);

		// Use wintype_focus, and treat WM windows and override-redirected
		// windows specially
		if (ps->o.wintype_option[w->window_type].focus ||
		    (ps->o.mark_wmwin_focused && w->wmwin) ||
		    (ps->o.mark_ovredir_focused && w->id == w->client_win && !w->wmwin) ||
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
	double opacity_tgt_old = w->opacity_tgt;
	w->opacity_tgt = win_calc_opacity_target(ps, w);
	if (opacity_tgt_old != w->opacity_tgt && w->state == WSTATE_MAPPED) {
		// Only MAPPED can transition to FADING
		assert(w->state != WSTATE_DESTROYING && w->state != WSTATE_UNMAPPING &&
		       w->state != WSTATE_UNMAPPED);
		w->state = WSTATE_FADING;
	}
}

/**
 * Set leader of a window.
 */
static inline void win_set_leader(session_t *ps, win *w, xcb_window_t nleader) {
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
void win_update_leader(session_t *ps, win *w) {
	xcb_window_t leader = XCB_NONE;

	// Read the leader properties
	if (ps->o.detect_transient && !leader)
		leader = wid_get_prop_window(ps, w->client_win, ps->atom_transient);

	if (ps->o.detect_client_leader && !leader)
		leader = wid_get_prop_window(ps, w->client_win, ps->atom_client_leader);

	win_set_leader(ps, w, leader);

	log_trace("(%#010x): client %#010x, leader %#010x, cache %#010x", w->id,
	          w->client_win, w->leader, win_get_leader(ps, w));
}

/**
 * Internal function of win_get_leader().
 */
xcb_window_t win_get_leader_raw(session_t *ps, win *w, int recursions) {
	// Rebuild the cache if needed
	if (!w->cache_leader && (w->client_win || w->leader)) {
		// Leader defaults to client window
		if (!(w->cache_leader = w->leader))
			w->cache_leader = w->client_win;

		// If the leader of this window isn't itself, look for its ancestors
		if (w->cache_leader && w->cache_leader != w->client_win) {
			win *wp = find_toplevel(ps, w->cache_leader);
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
bool win_get_class(session_t *ps, win *w) {
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
	if (!wid_get_text_prop(ps, w->client_win, ps->atom_class, &strlst, &nstr))
		return false;

	// Copy the strings if successful
	w->class_instance = strdup(strlst[0]);

	if (nstr > 1)
		w->class_general = strdup(strlst[1]);

	XFreeStringList(strlst);

	log_trace("(%#010x): client = %#010x, "
	          "instance = \"%s\", general = \"%s\"",
	          w->id, w->client_win, w->class_instance, w->class_general);

	return true;
}

/**
 * Handle window focus change.
 */
static void win_on_focus_change(session_t *ps, win *w) {
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
			cdbus_ev_win_focusin(ps, w);
		else
			cdbus_ev_win_focusout(ps, w);
	}
#endif
}

/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, win *w, bool focused) {
	// Unmapped windows will have their focused state reset on map
	if (w->a.map_state == XCB_MAP_STATE_UNMAPPED)
		return;

	if (win_is_focused_real(ps, w) == focused)
		return;

	if (focused) {
		if (ps->active_win)
			win_set_focused(ps, ps->active_win, false);
		ps->active_win = w;
	} else if (w == ps->active_win)
		ps->active_win = NULL;

	assert(win_is_focused_real(ps, w) == focused);

	win_on_focus_change(ps, w);
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(const win *w, region_t *res) {
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
void win_update_bounding_shape(session_t *ps, win *w) {
	if (ps->shape_exists)
		w->bounding_shaped = win_bounding_shaped(ps, w->id);

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
		    ps->c, xcb_shape_get_rectangles(ps->c, w->id, XCB_SHAPE_SK_BOUNDING), NULL);

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
	if (ps->o.experimental_backends) {
		// log_trace("free out dated pict");
		// Window shape changed, we should free win_data
		if (ps->redirected && w->state != WSTATE_UNMAPPED) {
			// Note we only do this when screen is redirected, because
			// otherwise win_data is not valid
			assert(w->state != WSTATE_UNMAPPING && w->state != WSTATE_DESTROYING);
			w->flags |= WIN_FLAGS_STALE_IMAGE;
		}
	} else {
		free_paint(ps, &w->paint);
		free_paint(ps, &w->shadow_paint);
	}

	win_on_factor_change(ps, w);
}

/**
 * Reread opacity property of a window.
 */
void win_update_opacity_prop(session_t *ps, win *w) {
	// get frame opacity first
	w->has_opacity_prop = wid_get_opacity_prop(ps, w->id, OPAQUE, &w->opacity_prop);

	if (w->has_opacity_prop)
		// opacity found
		return;

	if (ps->o.detect_client_opacity && w->client_win && w->id == w->client_win)
		// checking client opacity not allowed
		return;

	// get client opacity
	w->has_opacity_prop =
	    wid_get_opacity_prop(ps, w->client_win, OPAQUE, &w->opacity_prop);
}

/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(session_t *ps, win *w, xcb_window_t client) {
	winprop_t prop =
	    wid_get_prop(ps, client, ps->atom_frame_extents, 4L, XCB_ATOM_CARDINAL, 32);

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

	log_trace("(%#010x): %d, %d, %d, %d", w->id, w->frame_extents.left,
	          w->frame_extents.right, w->frame_extents.top, w->frame_extents.bottom);

	free_winprop(&prop);
}

bool win_is_region_ignore_valid(session_t *ps, const win *w) {
	WIN_STACK_ITER(ps, i) {
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
void win_ev_stop(session_t *ps, const win *w) {
	xcb_change_window_attributes(ps->c, w->id, XCB_CW_EVENT_MASK, (const uint32_t[]){0});

	if (w->client_win) {
		xcb_change_window_attributes(ps->c, w->client_win, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){0});
	}

	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c, w->id, 0);
	}
}

static void finish_unmap_win(session_t *ps, win **_w) {
	win *w = *_w;
	w->ever_damaged = false;
	w->reg_ignore_valid = false;
	w->state = WSTATE_UNMAPPED;

	if (ps->o.experimental_backends) {
		// We are in unmap_win, we definitely was viewable
		if (ps->redirected) {
			win_release_image(ps->backend_data, w);
		}
	} else {
		free_paint(ps, &w->paint);
		free_paint(ps, &w->shadow_paint);
	}

	w->flags = 0;
}

static void finish_destroy_win(session_t *ps, win **_w) {
	win *w = *_w;

	if (w->state != WSTATE_UNMAPPED) {
		// Only UNMAPPED state has window resources freed, otherwise
		// we need to call finish_unmap_win to free them.
		finish_unmap_win(ps, _w);
	}

	// Invalidate reg_ignore of windows below this one
	// TODO what if w->next is not mapped??
	// TODO seriously figure out how reg_ignore behaves.
	//      I think if `w` is unmapped, and destroyed after
	//      paint happened at least once, w->reg_ignore_valid would
	//      be true, and there is no need to invalid w->next->reg_ignore
	//      when w is destroyed.
	if (w->next) {
		rc_region_unref(&w->next->reg_ignore);
		w->next->reg_ignore_valid = false;
	}

	log_trace("Trying to destroy (%#010x)", w->id);
	*w->prev = w->next;
	if (w->next) {
		w->next->prev = w->prev;
	}

	if (w == ps->active_win) {
		ps->active_win = NULL;
	}

	free_win_res(ps, w);

	// Drop w from all prev_trans to avoid accessing freed memory in
	// repair_win()
	// TODO there can only be one prev_trans pointing to w
	WIN_STACK_ITER(ps, w2) {
		if (w == w2->prev_trans) {
			w2->prev_trans = NULL;
		}
	}
	free(w);
	*_w = NULL;
	return;
	log_warn("Destroyed window is not in window list");
	assert(false);
}

static void finish_map_win(session_t *ps, win **_w) {
	win *w = *_w;
	w->in_openclose = false;
	w->state = WSTATE_MAPPED;
}

void unmap_win(session_t *ps, win **_w, bool destroy) {
	win *w = *_w;

	winstate_t target_state = destroy ? WSTATE_DESTROYING : WSTATE_UNMAPPING;

	if (unlikely(!w)) {
		return;
	}

	if (!destroy && w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		// We don't care about mapping / unmapping of Input Only windows.
		// But we need to remember to destroy them, so future window with
		// the same id won't be handled incorrectly.
		// Issue #119 was caused by this.
		return;
	}

	log_trace("Unmapping %#010x \"%s\", destroy = %d", w->id, (w ? w->name : NULL), destroy);

	if (unlikely(w->state == WSTATE_DESTROYING && !destroy)) {
		log_warn("Trying to undestroy a window?");
		assert(false);
	}

	// If the window is already in the state we want
	if (unlikely(w->state == target_state)) {
		log_warn("%s a window twice", destroy ? "Destroying" : "Unmapping");
		return;
	}

	if (destroy) {
		// Delete destroyed window from the hash table, so future window with the
		// same window id won't confuse us.
		// Keep the window in the window stack, since we might still need to
		// render it (fading out). Window will be removed from the stack when
		// fading finishes.
		HASH_DEL(ps->windows, w);
	}

	if (unlikely(w->state == WSTATE_UNMAPPED) || w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		if (unlikely(!destroy)) {
			log_warn("Unmapping an already unmapped window %#010x %s twice",
			         w->id, w->name);
			return;
		}
		// Window is already unmapped, or is an Input Only window, just destroy it
		finish_destroy_win(ps, _w);
		return;
	}

	// Set focus out
	win_set_focused(ps, w, false);

	w->a.map_state = XCB_MAP_STATE_UNMAPPED;
	w->state = target_state;
	w->opacity_tgt = win_calc_opacity_target(ps, w);

	w->in_openclose = destroy;

	// don't care about properties anymore
	if (!destroy) {
		win_ev_stop(ps, w);
	}

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		if (destroy) {
			cdbus_ev_win_destroyed(ps, w);
		} else {
			cdbus_ev_win_unmapped(ps, w);
		}
	}
#endif

	if (!ps->redirected) {
		win_skip_fading(ps, _w);
	}
}

/**
 * Execute fade callback of a window if fading finished.
 */
void win_check_fade_finished(session_t *ps, win **_w) {
	win *w = *_w;
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		// No fading in progress
		assert(w->opacity_tgt == w->opacity);
		return;
	}
	if (w->opacity == w->opacity_tgt) {
		switch (w->state) {
		case WSTATE_UNMAPPING: return finish_unmap_win(ps, _w);
		case WSTATE_DESTROYING: return finish_destroy_win(ps, _w);
		case WSTATE_MAPPING: return finish_map_win(ps, _w);
		case WSTATE_FADING: w->state = WSTATE_MAPPED; break;
		default: unreachable;
		}
	}
}

/// Skip the current in progress fading of window,
/// transition the window straight to its end state
void win_skip_fading(session_t *ps, win **_w) {
	win *w = *_w;
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		assert(w->opacity_tgt == w->opacity);
		return;
	}
	log_trace("Skipping fading process of window %#010x (%s)", w->id, w->name);
	w->opacity = w->opacity_tgt;
	win_check_fade_finished(ps, _w);
}

/**
 * Get the Xinerama screen a window is on.
 *
 * Return an index >= 0, or -1 if not found.
 *
 * TODO move to x.c
 * TODO use xrandr
 */
void win_update_screen(session_t *ps, win *w) {
	w->xinerama_scr = -1;

	if (!ps->xinerama_scrs)
		return;

	xcb_xinerama_screen_info_t *scrs =
	    xcb_xinerama_query_screens_screen_info(ps->xinerama_scrs);
	int length = xcb_xinerama_query_screens_screen_info_length(ps->xinerama_scrs);
	for (int i = 0; i < length; i++) {
		xcb_xinerama_screen_info_t *s = &scrs[i];
		if (s->x_org <= w->g.x && s->y_org <= w->g.y &&
		    s->x_org + s->width >= w->g.x + w->widthb &&
		    s->y_org + s->height >= w->g.y + w->heightb) {
			w->xinerama_scr = i;
			return;
		}
	}
}

// TODO remove this
void configure_win(session_t *, xcb_configure_notify_event_t *);

/// Map an already registered window
void map_win(session_t *ps, win *w) {
	assert(w);

	// Don't care about window mapping if it's an InputOnly window
	// Also, try avoiding mapping a window twice
	if (w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		return;
	}

	log_debug("Mapping (%#010x \"%s\")", w->id, w->name);

	assert(w->state != WSTATE_DESTROYING);
	if (w->state != WSTATE_UNMAPPED && w->state != WSTATE_UNMAPPING) {
		log_warn("Mapping an already mapped window");
		return;
	}

	if (w->state == WSTATE_UNMAPPING) {
		win_skip_fading(ps, &w);
		// We skipped the unmapping process, the window was rendered, now it is
		// not anymore. So we need to mark then unmapping window as damaged.
		add_damage_from_win(ps, w);
		assert(w);
	}

	// We stopped processing window size change when we were unmapped, refresh the
	// size of the window
	xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(ps->c, w->id);
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(ps->c, gcookie, NULL);

	if (!g) {
		log_error("Failed to get the geometry of window %#010x", w->id);
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
	    ps->c, w->id, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, w->id, WIN_EVMODE_FRAME)});

	// Notify compton when the shape of a window changes
	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c, w->id, 1);
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

	log_debug("Window (%#010x) has type %s", w->id, WINTYPES[w->window_type]);

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
	w->opacity_tgt = win_calc_opacity_target(ps, w);

	log_debug("Window %#010x has opacity %f, opacity target is %f", w->id, w->opacity,
	          w->opacity_tgt);

	win_determine_blur_background(ps, w);

	w->ever_damaged = false;

	// We stopped listening on ShapeNotify events
	// when the window is unmapped (XXX we shouldn't),
	// so the shape of the window might have changed,
	// update. (Issue #35)
	win_update_bounding_shape(ps, w);

	// Reset the STALE_IMAGE flag set by win_update_bounding_shape. Because we are
	// just about to bind the image, no way that's stale.
	//
	// Also because NVIDIA driver doesn't like seeing the same pixmap under different
	// ids, so avoid naming the pixmap again when it didn't actually change.
	w->flags &= ~WIN_FLAGS_STALE_IMAGE;

	// Bind image after update_bounding_shape, so the shadow has the correct size.
	if (ps->redirected && ps->o.experimental_backends) {
		if (!win_bind_image(ps, w)) {
			w->flags |= WIN_FLAGS_IMAGE_ERROR;
		}
	}

#ifdef CONFIG_DBUS
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_mapped(ps, w);
	}
#endif

	if (!ps->redirected) {
		win_skip_fading(ps, &w);
		assert(w);
	}
}

void map_win_by_id(session_t *ps, xcb_window_t id) {
	// Unmap overlay window if it got mapped but we are currently not
	// in redirected state.
	if (ps->overlay && id == ps->overlay && !ps->redirected) {
		log_debug("Overlay is mapped while we are not redirected");
		auto e = xcb_request_check(ps->c, xcb_unmap_window(ps->c, ps->overlay));
		if (e) {
			log_error("Failed to unmap the overlay window");
			free(e);
		}
		// We don't track the overlay window, so we can return
		return;
	}

	win *w = find_win(ps, id);
	if (!w) {
		return;
	}

	map_win(ps, w);
}

/**
 * Find a window from window id in window linked list of the session.
 */
win *find_win(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	win *w = NULL;
	HASH_FIND_INT(ps->windows, &id, w);
	assert(w == NULL || w->state != WSTATE_DESTROYING);
	return w;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct win object of the found window, NULL if not found
 */
win *find_toplevel(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	HASH_ITER2(ps->windows, w) {
		assert(w->state != WSTATE_DESTROYING);
		if (w->client_win == id) {
			return w;
		}
	}

	return NULL;
}
