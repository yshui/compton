// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

#include "compiler.h"
#include "config.h"
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

// When image properties are actually applied to the image, they are applied in a
// particular order:
//
// Color inversion -> Dimming -> Opacity multiply -> Limit maximum brightness
// (Corner radius could be applied in any order)
enum image_properties {
	// Whether the color of the image is inverted
	// 1 boolean, default: false
	IMAGE_PROPERTY_INVERTED,
	// How much the image is dimmed
	// 1 double, default: 0
	IMAGE_PROPERTY_DIM_LEVEL,
	// Image opacity, i.e. an alpha value multiplied to the alpha channel
	// 1 double, default: 1
	IMAGE_PROPERTY_OPACITY,
	// The effective size of the image, the image will be tiled to fit.
	// 2 int, default: the actual size of the image
	IMAGE_PROPERTY_EFFECTIVE_SIZE,
	// Limit how bright image can be. The image brightness is estimated by averaging
	// the pixels in the image, and dimming will be applied to scale the average
	// brightness down to the max brightness value.
	// 1 double, default: 1
	IMAGE_PROPERTY_MAX_BRIGHTNESS,
	// Gives the image a rounded corner.
	// 1 double, default: 0
	IMAGE_PROPERTY_CORNER_RADIUS,
};

enum image_operations {
	// Multiply the alpha channel by the argument
	IMAGE_OP_APPLY_ALPHA,
};

struct gaussian_blur_args {
	int size;
	double deviation;
};

struct box_blur_args {
	int size;
};

struct kernel_blur_args {
	struct conv **kernels;
	int kernel_count;
};

struct dual_kawase_blur_args {
	int size;
	int strength;
};

struct backend_operations {
	// ===========    Initialization    ===========

	/// Initialize the backend, prepare for rendering to the target window.
	/// Here is how you should choose target window:
	///    1) if ps->overlay is not XCB_NONE, use that
	///    2) use ps->root otherwise
	// TODO(yshui) make the target window a parameter
	backend_t *(*init)(session_t *)attr_nonnull(1);
	void (*deinit)(backend_t *backend_data) attr_nonnull(1);

	/// Called when rendering will be stopped for an unknown amount of
	/// time (e.g. when screen is unredirected). Free some resources.
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

	// NOTE: due to complications introduced by use-damage and blur, the rendering API
	// is a bit weird. The idea is, `compose` and `blur` have to update a temporary
	// buffer, because `blur` requires data from an area slightly larger than the area
	// that will be visible. So the area outside the visible area has to be rendered,
	// but we have to discard the result (because the result of blurring that area
	// will be wrong). That's why we cannot render into the back buffer directly.
	// After rendering is done, `present` is called to update a portion of the actual
	// back buffer, then present it to the target (or update the target directly,
	// if not back buffered).

	/// Called before when a new frame starts.
	///
	/// Optional
	void (*prepare)(backend_t *backend_data, const region_t *reg_damage);

	/**
	 * Paint the content of an image onto the rendering buffer
	 *
	 * @param backend_data the backend data
	 * @param image_data   the image to paint
	 * @param dst_x, dst_y the top left corner of the image in the target
	 * @param reg_paint    the clip region, in target coordinates
	 * @param reg_visible  the visible region, in target coordinates
	 */
	void (*compose)(backend_t *backend_data, void *image_data, int dst_x, int dst_y,
	                const region_t *reg_paint, const region_t *reg_visible);

	/// Fill rectangle of the rendering buffer, mostly for debug purposes, optional.
	void (*fill)(backend_t *backend_data, struct color, const region_t *clip);

	/// Blur a given region of the rendering buffer.
	bool (*blur)(backend_t *backend_data, double opacity, void *blur_ctx,
	             const region_t *reg_blur, const region_t *reg_visible)
	    attr_nonnull(1, 3, 4, 5);

	/// Update part of the back buffer with the rendering buffer, then present the
	/// back buffer onto the target window (if not back buffered, update part of the
	/// target window directly).
	///
	/// Optional, if NULL, indicates the backend doesn't have render output
	///
	/// @param region part of the target that should be updated
	void (*present)(backend_t *backend_data, const region_t *region) attr_nonnull(1, 2);

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

	/* TODO(yshui) Consider preserving the order of image ops.
	 * Currently in both backends, the image ops are applied lazily when needed.
	 * However neither backends preserve the order of image ops, they just applied all
	 * pending lazy ops in a pre-determined fixed order, regardless in which order
	 * they were originally applied. This might lead to inconsistencies.*/

	/**
	 * Change image properties
	 *
	 * @param backend_data backend data
	 * @param prop         the property to change
	 * @param image_data   an image data structure returned by the backend
	 * @param args         property value
	 * @return whether the operation is successful
	 */
	bool (*set_image_property)(backend_t *backend_data, enum image_properties prop,
	                           void *image_data, void *args);

	/**
	 * Manipulate an image. Image properties are untouched.
	 *
	 * @param backend_data backend data
	 * @param op           the operation to perform
	 * @param image_data   an image data structure returned by the backend
	 * @param reg_op       the clip region, define the part of the image to be
	 *                     operated on.
	 * @param reg_visible  define the part of the image that will eventually
	 *                     be visible on target. this is a hint to the backend
	 *                     for optimization purposes.
	 * @param args         extra arguments, operation specific
	 * @return whether the operation is successful
	 */
	bool (*image_op)(backend_t *backend_data, enum image_operations op, void *image_data,
	                 const region_t *reg_op, const region_t *reg_visible, void *args);

	/**
	 * Read the color of the pixel at given position of the given image. Image
	 * properties have no effect.
	 *
	 * @param      backend_data backend_data
	 * @param      image_data   an image data structure previously returned by the
	 *                          backend. the image to read pixel from.
	 * @param      x, y         coordinate of the pixel to read
	 * @param[out] color        the color of the pixel
	 * @return whether the operation is successful
	 */
	bool (*read_pixel)(backend_t *backend_data, void *image_data, int x, int y,
	                   struct color *output);

	/// Create another instance of the `image_data`. All `image_op` and
	/// `set_image_property` calls on the returned image should not affect the
	/// original image
	void *(*clone_image)(backend_t *base, const void *image_data,
	                     const region_t *reg_visible);

	/// Create a blur context that can be used to call `blur`
	void *(*create_blur_context)(backend_t *base, enum blur_method, void *args);
	/// Destroy a blur context
	void (*destroy_blur_context)(backend_t *base, void *ctx);
	/// Get how many pixels outside of the blur area is needed for blur
	void (*get_blur_size)(void *blur_context, int *width, int *height);

	// ===========         Hooks        ============
	/// Let the backend hook into the event handling queue
	/// Not implemented yet
	void (*set_ready_callback)(backend_t *, backend_ready_callback_t cb);
	/// Called right after the core has handled its events.
	/// Not implemented yet
	void (*handle_events)(backend_t *);
	// ===========         Misc         ============
	/// Return the driver that is been used by the backend
	enum driver (*detect_driver)(backend_t *backend_data);

	void (*diagnostics)(backend_t *backend_data);
};

extern struct backend_operations *backend_list[];

void paint_all_new(session_t *ps, struct managed_win *const t, bool ignore_damage)
    attr_nonnull(1);
