// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#include <stdbool.h>
#include <stdlib.h>

#include <X11/Xutil.h>
#include <pixman.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>

#include "atom.h"
#include "backend/gl/glx.h"
#include "common.h"
#include "compiler.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "utils.h"
#include "x.h"

/**
 * Get a specific attribute of a window.
 *
 * Returns a blank structure if the returned type and format does not
 * match the requested type and format.
 *
 * @param ps current session
 * @param w window
 * @param atom atom of attribute to fetch
 * @param length length to read
 * @param rtype atom of the requested type
 * @param rformat requested format
 * @return a <code>winprop_t</code> structure containing the attribute
 *    and number of items. A blank one on failure.
 */
winprop_t wid_get_prop_adv(const session_t *ps, xcb_window_t w, xcb_atom_t atom,
                           int offset, int length, xcb_atom_t rtype, int rformat) {
	xcb_get_property_reply_t *r = xcb_get_property_reply(
	    ps->c,
	    xcb_get_property(ps->c, 0, w, atom, rtype, to_u32_checked(offset),
	                     to_u32_checked(length)),
	    NULL);

	if (r && xcb_get_property_value_length(r) &&
	    (rtype == XCB_GET_PROPERTY_TYPE_ANY || r->type == rtype) &&
	    (!rformat || r->format == rformat) &&
	    (r->format == 8 || r->format == 16 || r->format == 32)) {
		auto len = xcb_get_property_value_length(r);
		return (winprop_t){
		    .ptr = xcb_get_property_value(r),
		    .nitems = (ulong)(len / (r->format / 8)),
		    .type = r->type,
		    .format = r->format,
		    .r = r,
		};
	}

	free(r);
	return (winprop_t){
	    .ptr = NULL, .nitems = 0, .type = XCB_GET_PROPERTY_TYPE_ANY, .format = 0};
}

/**
 * Get the value of a type-<code>xcb_window_t</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
xcb_window_t wid_get_prop_window(session_t *ps, xcb_window_t wid, xcb_atom_t aprop) {
	// Get the attribute
	xcb_window_t p = XCB_NONE;
	winprop_t prop = wid_get_prop(ps, wid, aprop, 1L, XCB_ATOM_WINDOW, 32);

	// Return it
	if (prop.nitems) {
		p = (xcb_window_t)*prop.p32;
	}

	free_winprop(&prop);

	return p;
}

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(session_t *ps, xcb_window_t wid, xcb_atom_t prop, char ***pstrlst,
                       int *pnstr) {
	XTextProperty text_prop = {NULL, XCB_NONE, 0, 0};

	if (!(XGetTextProperty(ps->dpy, wid, &text_prop, prop) && text_prop.value))
		return false;

	if (Success != XmbTextPropertyToTextList(ps->dpy, &text_prop, pstrlst, pnstr) ||
	    !*pnstr) {
		*pnstr = 0;
		if (*pstrlst)
			XFreeStringList(*pstrlst);
		XFree(text_prop.value);
		return false;
	}

	XFree(text_prop.value);
	return true;
}

// A cache of pict formats. We assume they don't change during the lifetime
// of compton
static thread_local xcb_render_query_pict_formats_reply_t *g_pictfmts = NULL;

static inline void x_get_server_pictfmts(xcb_connection_t *c) {
	if (g_pictfmts)
		return;
	xcb_generic_error_t *e = NULL;
	// Get window picture format
	g_pictfmts =
	    xcb_render_query_pict_formats_reply(c, xcb_render_query_pict_formats(c), &e);
	if (e || !g_pictfmts) {
		log_fatal("failed to get pict formats\n");
		abort();
	}
}

const xcb_render_pictforminfo_t *
x_get_pictform_for_visual(xcb_connection_t *c, xcb_visualid_t visual) {
	x_get_server_pictfmts(c);

	xcb_render_pictvisual_t *pv = xcb_render_util_find_visual_format(g_pictfmts, visual);
	for (xcb_render_pictforminfo_iterator_t i =
	         xcb_render_query_pict_formats_formats_iterator(g_pictfmts);
	     i.rem; xcb_render_pictforminfo_next(&i)) {
		if (i.data->id == pv->format) {
			return i.data;
		}
	}
	return NULL;
}

static xcb_visualid_t attr_pure x_get_visual_for_pictfmt(xcb_render_query_pict_formats_reply_t *r,
                                                         xcb_render_pictformat_t fmt) {
	for (auto screen = xcb_render_query_pict_formats_screens_iterator(r); screen.rem;
	     xcb_render_pictscreen_next(&screen)) {
		for (auto depth = xcb_render_pictscreen_depths_iterator(screen.data);
		     depth.rem; xcb_render_pictdepth_next(&depth)) {
			for (auto pv = xcb_render_pictdepth_visuals_iterator(depth.data);
			     pv.rem; xcb_render_pictvisual_next(&pv)) {
				if (pv.data->format == fmt) {
					return pv.data->visual;
				}
			}
		}
	}
	return XCB_NONE;
}

xcb_visualid_t x_get_visual_for_standard(xcb_connection_t *c, xcb_pict_standard_t std) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, std);

	return x_get_visual_for_pictfmt(g_pictfmts, pictfmt->id);
}

int x_get_visual_depth(xcb_connection_t *c, xcb_visualid_t visual) {
	auto setup = xcb_get_setup(c);
	for (auto screen = xcb_setup_roots_iterator(setup); screen.rem;
	     xcb_screen_next(&screen)) {
		for (auto depth = xcb_screen_allowed_depths_iterator(screen.data);
		     depth.rem; xcb_depth_next(&depth)) {
			const int len = xcb_depth_visuals_length(depth.data);
			const xcb_visualtype_t *visuals = xcb_depth_visuals(depth.data);
			for (int i = 0; i < len; i++) {
				if (visual == visuals[i].visual_id) {
					return depth.data->depth;
				}
			}
		}
	}
	return -1;
}

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(xcb_connection_t *c,
                                         const xcb_render_pictforminfo_t *pictfmt,
                                         xcb_pixmap_t pixmap, uint32_t valuemask,
                                         const xcb_render_create_picture_value_list_t *attr) {
	void *buf = NULL;
	if (attr) {
		xcb_render_create_picture_value_list_serialize(&buf, valuemask, attr);
		if (!buf) {
			log_error("failed to serialize picture attributes");
			return XCB_NONE;
		}
	}

	xcb_render_picture_t tmp_picture = x_new_id(c);
	xcb_generic_error_t *e =
	    xcb_request_check(c, xcb_render_create_picture_checked(
	                             c, tmp_picture, pixmap, pictfmt->id, valuemask, buf));
	free(buf);
	if (e) {
		x_print_error(e->full_sequence, e->major_code, e->minor_code, e->error_code);
		log_error("failed to create picture");
		return XCB_NONE;
	}
	return tmp_picture;
}

xcb_render_picture_t
x_create_picture_with_visual_and_pixmap(xcb_connection_t *c, xcb_visualid_t visual,
                                        xcb_pixmap_t pixmap, uint32_t valuemask,
                                        const xcb_render_create_picture_value_list_t *attr) {
	const xcb_render_pictforminfo_t *pictfmt = x_get_pictform_for_visual(c, visual);
	return x_create_picture_with_pictfmt_and_pixmap(c, pictfmt, pixmap, valuemask, attr);
}

xcb_render_picture_t
x_create_picture_with_standard_and_pixmap(xcb_connection_t *c, xcb_pict_standard_t standard,
                                          xcb_pixmap_t pixmap, uint32_t valuemask,
                                          const xcb_render_create_picture_value_list_t *attr) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, standard);
	assert(pictfmt);
	return x_create_picture_with_pictfmt_and_pixmap(c, pictfmt, pixmap, valuemask, attr);
}

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture_with_pictfmt(xcb_connection_t *c, xcb_drawable_t d, int w, int h,
                              const xcb_render_pictforminfo_t *pictfmt, uint32_t valuemask,
                              const xcb_render_create_picture_value_list_t *attr) {
	uint8_t depth = pictfmt->depth;

	xcb_pixmap_t tmp_pixmap = x_create_pixmap(c, depth, d, w, h);
	if (!tmp_pixmap)
		return XCB_NONE;

	xcb_render_picture_t picture = x_create_picture_with_pictfmt_and_pixmap(
	    c, pictfmt, tmp_pixmap, valuemask, attr);

	xcb_free_pixmap(c, tmp_pixmap);

	return picture;
}

xcb_render_picture_t
x_create_picture_with_visual(xcb_connection_t *c, xcb_drawable_t d, int w, int h,
                             xcb_visualid_t visual, uint32_t valuemask,
                             const xcb_render_create_picture_value_list_t *attr) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	return x_create_picture_with_pictfmt(c, d, w, h, pictfmt, valuemask, attr);
}

bool x_fetch_region(xcb_connection_t *c, xcb_xfixes_region_t r, pixman_region32_t *res) {
	xcb_generic_error_t *e = NULL;
	xcb_xfixes_fetch_region_reply_t *xr =
	    xcb_xfixes_fetch_region_reply(c, xcb_xfixes_fetch_region(c, r), &e);
	if (!xr) {
		log_error("Failed to fetch rectangles");
		return false;
	}

	int nrect = xcb_xfixes_fetch_region_rectangles_length(xr);
	auto b = ccalloc(nrect, pixman_box32_t);
	xcb_rectangle_t *xrect = xcb_xfixes_fetch_region_rectangles(xr);
	for (int i = 0; i < nrect; i++) {
		b[i] = (pixman_box32_t){.x1 = xrect[i].x,
		                        .y1 = xrect[i].y,
		                        .x2 = xrect[i].x + xrect[i].width,
		                        .y2 = xrect[i].y + xrect[i].height};
	}
	bool ret = pixman_region32_init_rects(res, b, nrect);
	free(b);
	free(xr);
	return ret;
}

void x_set_picture_clip_region(xcb_connection_t *c, xcb_render_picture_t pict,
                               int16_t clip_x_origin, int16_t clip_y_origin,
                               const region_t *reg) {
	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);
	auto xrects = ccalloc(nrects, xcb_rectangle_t);
	for (int i = 0; i < nrects; i++)
		xrects[i] = (xcb_rectangle_t){
		    .x = to_i16_checked(rects[i].x1),
		    .y = to_i16_checked(rects[i].y1),
		    .width = to_u16_checked(rects[i].x2 - rects[i].x1),
		    .height = to_u16_checked(rects[i].y2 - rects[i].y1),
		};

	xcb_generic_error_t *e = xcb_request_check(
	    c, xcb_render_set_picture_clip_rectangles_checked(
	           c, pict, clip_x_origin, clip_y_origin, to_u32_checked(nrects), xrects));
	if (e)
		log_error("Failed to set clip region");
	free(e);
	free(xrects);
	return;
}

void x_clear_picture_clip_region(xcb_connection_t *c, xcb_render_picture_t pict) {
	xcb_render_change_picture_value_list_t v = {.clipmask = XCB_NONE};
	xcb_generic_error_t *e = xcb_request_check(
	    c, xcb_render_change_picture(c, pict, XCB_RENDER_CP_CLIP_MASK, &v));
	if (e)
		log_error("failed to clear clip region");
	free(e);
	return;
}

enum { XSyncBadCounter = 0,
       XSyncBadAlarm = 1,
       XSyncBadFence = 2,
};

/**
 * X11 error handler function.
 *
 * XXX consider making this error to string
 */
void x_print_error(unsigned long serial, uint8_t major, uint16_t minor, uint8_t error_code) {
	session_t *const ps = ps_g;

	int o = 0;
	const char *name = "Unknown";

	if (major == ps->composite_opcode && minor == XCB_COMPOSITE_REDIRECT_SUBWINDOWS) {
		log_fatal("Another composite manager is already running "
		          "(and does not handle _NET_WM_CM_Sn correctly)");
		exit(1);
	}

#define CASESTRRET2(s)                                                                   \
	case s: name = #s; break

	o = error_code - ps->xfixes_error;
	switch (o) { CASESTRRET2(XCB_XFIXES_BAD_REGION); }

	o = error_code - ps->damage_error;
	switch (o) { CASESTRRET2(XCB_DAMAGE_BAD_DAMAGE); }

	o = error_code - ps->render_error;
	switch (o) {
		CASESTRRET2(XCB_RENDER_PICT_FORMAT);
		CASESTRRET2(XCB_RENDER_PICTURE);
		CASESTRRET2(XCB_RENDER_PICT_OP);
		CASESTRRET2(XCB_RENDER_GLYPH_SET);
		CASESTRRET2(XCB_RENDER_GLYPH);
	}

	if (ps->xsync_exists) {
		o = error_code - ps->xsync_error;
		switch (o) {
			CASESTRRET2(XSyncBadCounter);
			CASESTRRET2(XSyncBadAlarm);
			CASESTRRET2(XSyncBadFence);
		}
	}

	switch (error_code) {
		CASESTRRET2(BadAccess);
		CASESTRRET2(BadAlloc);
		CASESTRRET2(BadAtom);
		CASESTRRET2(BadColor);
		CASESTRRET2(BadCursor);
		CASESTRRET2(BadDrawable);
		CASESTRRET2(BadFont);
		CASESTRRET2(BadGC);
		CASESTRRET2(BadIDChoice);
		CASESTRRET2(BadImplementation);
		CASESTRRET2(BadLength);
		CASESTRRET2(BadMatch);
		CASESTRRET2(BadName);
		CASESTRRET2(BadPixmap);
		CASESTRRET2(BadRequest);
		CASESTRRET2(BadValue);
		CASESTRRET2(BadWindow);
	}

#undef CASESTRRET2

	log_debug("X error %d %s request %d minor %d serial %lu", error_code, name, major,
	          minor, serial);
}

/**
 * Create a pixmap and check that creation succeeded.
 */
xcb_pixmap_t x_create_pixmap(xcb_connection_t *c, uint8_t depth, xcb_drawable_t drawable,
                             int width, int height) {
	xcb_pixmap_t pix = x_new_id(c);
	xcb_void_cookie_t cookie = xcb_create_pixmap_checked(
	    c, depth, pix, drawable, to_u16_checked(width), to_u16_checked(height));
	xcb_generic_error_t *err = xcb_request_check(c, cookie);
	if (err == NULL)
		return pix;

	log_error("Failed to create pixmap:");
	x_print_error(err->sequence, err->major_code, err->minor_code, err->error_code);
	free(err);
	return XCB_NONE;
}

/**
 * Validate a pixmap.
 *
 * Detect whether the pixmap is valid with XGetGeometry. Well, maybe there
 * are better ways.
 */
bool x_validate_pixmap(xcb_connection_t *c, xcb_pixmap_t pixmap) {
	if (pixmap == XCB_NONE) {
		return false;
	}

	auto r = xcb_get_geometry_reply(c, xcb_get_geometry(c, pixmap), NULL);
	if (!r) {
		return false;
	}

	bool ret = r->width && r->height;
	free(r);
	return ret;
}
/// Names of root window properties that could point to a pixmap of
/// background.
static const char *background_props_str[] = {
    "_XROOTPMAP_ID",
    "_XSETROOT_ID",
    0,
};

xcb_pixmap_t x_get_root_back_pixmap(session_t *ps) {
	xcb_pixmap_t pixmap = XCB_NONE;

	// Get the values of background attributes
	for (int p = 0; background_props_str[p]; p++) {
		xcb_atom_t prop_atom = get_atom(ps->atoms, background_props_str[p]);
		winprop_t prop =
		    wid_get_prop(ps, ps->root, prop_atom, 1, XCB_ATOM_PIXMAP, 32);
		if (prop.nitems) {
			pixmap = (xcb_pixmap_t)*prop.p32;
			free_winprop(&prop);
			break;
		}
		free_winprop(&prop);
	}

	return pixmap;
}

bool x_is_root_back_pixmap_atom(session_t *ps, xcb_atom_t atom) {
	for (int p = 0; background_props_str[p]; p++) {
		xcb_atom_t prop_atom = get_atom(ps->atoms, background_props_str[p]);
		if (prop_atom == atom)
			return true;
	}
	return false;
}

/**
 * Synchronizes a X Render drawable to ensure all pending painting requests
 * are completed.
 */
bool x_fence_sync(xcb_connection_t *c, xcb_sync_fence_t f) {
	// TODO(richardgv): If everybody just follows the rules stated in X Sync
	// prototype, we need only one fence per screen, but let's stay a bit
	// cautious right now

	auto e = xcb_request_check(c, xcb_sync_trigger_fence_checked(c, f));
	if (e) {
		log_error("Failed to trigger the fence.");
		free(e);
		return false;
	}

	e = xcb_request_check(c, xcb_sync_await_fence_checked(c, 1, &f));
	if (e) {
		log_error("Failed to await on a fence.");
		free(e);
		return false;
	}

	e = xcb_request_check(c, xcb_sync_reset_fence_checked(c, f));
	if (e) {
		log_error("Failed to reset the fence.");
		free(e);
		return false;
	}
	return true;
}

// xcb-render specific macros
#define XFIXED_TO_DOUBLE(value) (((double)(value)) / 65536)
#define DOUBLE_TO_XFIXED(value) ((xcb_render_fixed_t)(((double)(value)) * 65536))

/**
 * Convert a struct conv to a X picture convolution filter, normalizing the kernel
 * in the process. Allow the caller to specify the element at the center of the kernel,
 * for compatibility with legacy code.
 *
 * @param[in] kernel the convolution kernel
 * @param[in] center the element to put at the center of the matrix
 * @param[inout] ret pointer to an array of `size`, if `size` is too small, more space
 *                   will be allocated, and `*ret` will be updated
 * @param[inout] size size of the array pointed to by `ret`, in number of elements
 * @return number of elements filled into `*ret`
 */
void x_create_convolution_kernel(const conv *kernel, double center,
                                 struct x_convolution_kernel **ret) {
	assert(ret);
	if (!*ret || (*ret)->capacity < kernel->w * kernel->h + 2) {
		free(*ret);
		*ret =
		    cvalloc(sizeof(struct x_convolution_kernel) +
		            (size_t)(kernel->w * kernel->h + 2) * sizeof(xcb_render_fixed_t));
		(*ret)->capacity = kernel->w * kernel->h + 2;
	}

	(*ret)->size = kernel->w * kernel->h + 2;

	auto buf = (*ret)->kernel;
	buf[0] = DOUBLE_TO_XFIXED(kernel->w);
	buf[1] = DOUBLE_TO_XFIXED(kernel->h);

	double sum = center;
	for (int i = 0; i < kernel->w * kernel->h; i++) {
		if (i == kernel->w * kernel->h / 2) {
			continue;
		}
		sum += kernel->data[i];
	}

	// Note for floating points a / b != a * (1 / b), but this shouldn't have any real
	// impact on the result
	double factor = sum != 0 ? 1.0 / sum : 1;
	for (int i = 0; i < kernel->w * kernel->h; i++) {
		buf[i + 2] = DOUBLE_TO_XFIXED(kernel->data[i] * factor);
	}

	buf[kernel->h / 2 * kernel->w + kernel->w / 2 + 2] =
	    DOUBLE_TO_XFIXED(center * factor);
}

/// Generate a search criteria for fbconfig from a X visual.
/// Returns {-1, -1, -1, -1, -1, 0} on failure
struct xvisual_info x_get_visual_info(xcb_connection_t *c, xcb_visualid_t visual) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	auto depth = x_get_visual_depth(c, visual);
	if (!pictfmt || depth == -1) {
		log_error("Invalid visual %#03x", visual);
		return (struct xvisual_info){-1, -1, -1, -1, -1, 0};
	}
	if (pictfmt->type != XCB_RENDER_PICT_TYPE_DIRECT) {
		log_error("compton cannot handle non-DirectColor visuals. Report an "
		          "issue if you see this error message.");
		return (struct xvisual_info){-1, -1, -1, -1, -1, 0};
	}

	int red_size = popcountl(pictfmt->direct.red_mask),
	    blue_size = popcountl(pictfmt->direct.blue_mask),
	    green_size = popcountl(pictfmt->direct.green_mask),
	    alpha_size = popcountl(pictfmt->direct.alpha_mask);

	return (struct xvisual_info){
	    .red_size = red_size,
	    .green_size = green_size,
	    .blue_size = blue_size,
	    .alpha_size = alpha_size,
	    .visual_depth = depth,
	    .visual = visual,
	};
}

xcb_screen_t *x_screen_of_display(xcb_connection_t *c, int screen) {
	xcb_screen_iterator_t iter;

	iter = xcb_setup_roots_iterator(xcb_get_setup(c));
	for (; iter.rem; --screen, xcb_screen_next(&iter))
		if (screen == 0)
			return iter.data;

	return NULL;
}
