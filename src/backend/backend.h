// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

#include "compiler.h"
#include "driver.h"
#include "kernel.h"
#include "region.h"
#include "types.h"
#include "x.h"

typedef struct session session_t;
struct managed_win;

struct ev_loop;
struct backend_operations;

typedef struct backend_base {
	struct backend_operations *ops;
	xcb_connection_t *c;
	xcb_window_t root;
	struct ev_loop *loop;

	/// Whether the backend can accept new render request at the moment
	bool busy;
	// ...
} backend_t;

typedef void (*backend_ready_callback_t)(void *);

enum image_operations {
	// Invert the color of the entire image, `reg_op` ignored
	IMAGE_OP_INVERT_COLOR_ALL,
	// Dim the entire image, argument is the percentage. `reg_op` ignored
	IMAGE_OP_DIM_ALL,
	// Multiply the alpha channel by the argument
	IMAGE_OP_APPLY_ALPHA,
	// Same as APPLY_ALPHA, but `reg_op` is ignored and the operation applies to the
	// full image
	IMAGE_OP_APPLY_ALPHA_ALL,
	// Change the effective size of the image, without touching the backing image
	// itself. When the image is used, the backing image should be tiled to fill its
	// effective size. `reg_op` and `reg_visible` is ignored. `arg` is two integers,
	// width and height, in that order.
	IMAGE_OP_RESIZE_TILE,
};

struct backend_operations {

	// ===========    Initialization    ===========

	/// Initialize the backend, prepare for rendering to the target window.
	/// Here is how you should choose target window:
	///    1) if ps->overlay is not XCB_NONE, use that
	///    2) use ps->root otherwise
	/// TODO make the target window a parameter
	backend_t *(*init)(session_t *)attr_nonnull(1);
	void (*deinit)(backend_t *backend_data) attr_nonnull(1);

	/// Called when rendering will be stopped for an unknown amount of
	/// time (e.g. screen is unredirected). Free some resources.
	///
	/// Optional, not yet used
	void (*pause)(backend_t *backend_data, session_t *ps);

	/// Called before rendering is resumed
	///
	/// Optional, not yet used
	void (*resume)(backend_t *backend_data, session_t *ps);

	/// Called when root property changed, returns the new
	/// backend_data. Even if the backend_data changed, all
	/// the existing image data returned by this backend should
	/// remain valid.
	///
	/// Optional
	void *(*root_change)(backend_t *backend_data, session_t *ps);

	// ===========      Rendering      ============

	// NOTE: general idea about reg_paint/reg_op vs reg_visible is that reg_visible is
	// merely a hint. Ignoring reg_visible entirely don't affect the correctness of
	// the operation performed. OTOH reg_paint/reg_op is part of the parameters of the
	// operation, and must be honored in order to complete the operation correctly.

	/// Called before when a new frame starts.
	///
	/// Optional
	void (*prepare)(backend_t *backend_data, const region_t *reg_damage);

	/**
	 * Paint the content of an image onto the (possibly buffered)
	 * target picture.
	 *
	 * @param backend_data the backend data
	 * @param image_data   the image to paint
	 * @param dst_x, dst_y the top left corner of the image in the target
	 * @param reg_paint    the clip region, in target coordinates
	 * @param reg_visible the visible region, in target coordinates
	 */
	void (*compose)(backend_t *backend_data, void *image_data, int dst_x, int dst_y,
	                const region_t *reg_paint, const region_t *reg_visible);

	/// Fill rectangle of target, mostly for debug purposes, optional.
	void (*fill)(backend_t *backend_data, struct color, const region_t *clip);

	/// Blur a given region of the target.
	bool (*blur)(backend_t *backend_data, double opacity, const region_t *reg_blur,
	             const region_t *reg_visible) attr_nonnull(1, 3, 4);

	/// Present the back buffer onto the screen.
	///
	/// Optional if the screen is not buffered
	void (*present)(backend_t *backend_data) attr_nonnull(1);

	/**
	 * Bind a X pixmap to the backend's internal image data structure.
	 *
	 * @param backend_data backend data
	 * @param pixmap X pixmap to bind
	 * @param fmt information of the pixmap's visual
	 * @param owned whether the ownership of the pixmap is transfered to the backend
	 * @return backend internal data structure bound with this pixmap
	 */
	void *(*bind_pixmap)(backend_t *backend_data, xcb_pixmap_t pixmap,
	                     struct xvisual_info fmt, bool owned);

	/// Create a shadow image based on the parameters
	/// Default implementation: default_backend_render_shadow
	void *(*render_shadow)(backend_t *backend_data, int width, int height,
	                       const conv *kernel, double r, double g, double b, double a);

	// ============ Resource management ===========

	// XXX Thoughts: calling release_image and render_* for every config notify
	//     is wasteful, since there can be multiple such notifies per drawing.
	//     But if we don't, it can mean there will be a state where is window is
	//     mapped and visible, but there is no win_data attached to it. We don't
	//     want to break that assumption as for now. We need to reconsider this.

	/// Free resources associated with an image data structure
	void (*release_image)(backend_t *backend_data, void *img_data) attr_nonnull(1, 2);

	// ===========        Query         ===========

	/// Return if image is not completely opaque.
	///
	/// This function is needed because some backend might change the content of the
	/// window (e.g. when using a custom shader with the glx backend), so only the
	/// backend knows if an image is transparent.
	bool (*is_image_transparent)(backend_t *backend_data, void *image_data)
	    attr_nonnull(1, 2);

	/// Get the age of the buffer content we are currently rendering ontop
	/// of. The buffer that has just been `present`ed has a buffer age of 1.
	/// Everytime `present` is called, buffers get older. Return -1 if the
	/// buffer is empty.
	///
	/// Optional
	int (*buffer_age)(backend_t *backend_data);

	/// The maximum number buffer_age might return.
	int max_buffer_age;

	// ===========    Post-processing   ============
	/**
	 * Manipulate an image
	 *
	 * @param backend_data backend data
	 * @param op           the operation to perform
	 * @param image_data   an image data structure returned by the backend
	 * @param reg_op       the clip region, define the part of the image to be
	 *                     operated on.
	 * @param reg_visible  define the part of the image that will eventually
	 *                     be visible on screen. this is a hint to the backend
	 *                     for optimization purposes.
	 * @param args         extra arguments, operation specific
	 * @return a new image data structure containing the result
	 */
	bool (*image_op)(backend_t *backend_data, enum image_operations op, void *image_data,
	                 const region_t *reg_op, const region_t *reg_visible, void *args);

	/// Create another instance of the `image_data`. All `image_op` calls on the
	/// returned image should not affect the original image
	void *(*copy)(backend_t *base, const void *image_data, const region_t *reg_visible);

	// ===========         Hooks        ============
	/// Let the backend hook into the event handling queue
	void (*set_ready_callback)(backend_t *, backend_ready_callback_t cb);
	/// Called right after compton has handled its events.
	void (*handle_events)(backend_t *);
	// ===========         Misc         ============
	/// Return the driver that is been used by the backend
	enum driver (*detect_driver)(backend_t *backend_data);
};

typedef backend_t *(*backend_init_fn)(session_t *ps)attr_nonnull(1);

extern struct backend_operations *backend_list[];

void paint_all_new(session_t *ps, struct managed_win *const t, bool ignore_damage)
    attr_nonnull(1);

// vim: set noet sw=8 ts=8 :
