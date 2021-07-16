// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <math.h>
#include <string.h>
#include <xcb/render.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_renderutil.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "utils.h"
#include "win.h"
#include "x.h"

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
xcb_render_picture_t solid_picture(xcb_connection_t *c, xcb_drawable_t d, bool argb,
                                   double a, double r, double g, double b) {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t picture;
	xcb_render_create_picture_value_list_t pa;
	xcb_render_color_t col;
	xcb_rectangle_t rect;

	pixmap = x_create_pixmap(c, argb ? 32 : 8, d, 1, 1);
	if (!pixmap)
		return XCB_NONE;

	pa.repeat = 1;
	picture = x_create_picture_with_standard_and_pixmap(
	    c, argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
	    XCB_RENDER_CP_REPEAT, &pa);

	if (!picture) {
		xcb_free_pixmap(c, pixmap);
		return XCB_NONE;
	}

	col.alpha = (uint16_t)(a * 0xffff);
	col.red = (uint16_t)(r * 0xffff);
	col.green = (uint16_t)(g * 0xffff);
	col.blue = (uint16_t)(b * 0xffff);

	rect.x = 0;
	rect.y = 0;
	rect.width = 1;
	rect.height = 1;

	xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, picture, col, 1, &rect);
	xcb_free_pixmap(c, pixmap);

	return picture;
}

xcb_image_t *
make_shadow(xcb_connection_t *c, const conv *kernel, double opacity, int width, int height) {
	/*
	 * We classify shadows into 4 kinds of regions
	 *    r = shadow radius
	 * (0, 0) is the top left of the window itself
	 *         -r     r      width-r  width+r
	 *       -r +-----+---------+-----+
	 *          |  1  |    2    |  1  |
	 *        r +-----+---------+-----+
	 *          |  2  |    3    |  2  |
	 * height-r +-----+---------+-----+
	 *          |  1  |    2    |  1  |
	 * height+r +-----+---------+-----+
	 */
	xcb_image_t *ximage;
	const double *shadow_sum = kernel->rsum;
	assert(shadow_sum);
	// We only support square kernels for shadow
	assert(kernel->w == kernel->h);
	int d = kernel->w;
	int r = d / 2;
	int swidth = width + r * 2, sheight = height + r * 2;

	assert(d % 2 == 1);
	assert(d > 0);

	ximage = xcb_image_create_native(c, to_u16_checked(swidth), to_u16_checked(sheight),
	                                 XCB_IMAGE_FORMAT_Z_PIXMAP, 8, 0, 0, NULL);
	if (!ximage) {
		log_error("failed to create an X image");
		return 0;
	}

	unsigned char *data = ximage->data;
	long sstride = ximage->stride;

	// If the window body is smaller than the kernel, we do convolution directly
	if (width < r * 2 && height < r * 2) {
		for (int y = 0; y < sheight; y++) {
			for (int x = 0; x < swidth; x++) {
				double sum = sum_kernel_normalized(
				    kernel, d - x - 1, d - y - 1, width, height);
				data[y * sstride + x] = (uint8_t)(sum * 255.0);
			}
		}
		return ximage;
	}

	if (height < r * 2) {
		// Implies width >= r * 2
		// If the window height is smaller than the kernel, we divide
		// the window like this:
		// -r     r         width-r  width+r
		// +------+-------------+------+
		// |      |             |      |
		// +------+-------------+------+
		for (int y = 0; y < sheight; y++) {
			for (int x = 0; x < r * 2; x++) {
				double sum = sum_kernel_normalized(kernel, d - x - 1,
				                                   d - y - 1, d, height) *
				             255.0;
				data[y * sstride + x] = (uint8_t)sum;
				data[y * sstride + swidth - x - 1] = (uint8_t)sum;
			}
		}
		for (int y = 0; y < sheight; y++) {
			double sum =
			    sum_kernel_normalized(kernel, 0, d - y - 1, d, height) * 255.0;
			memset(&data[y * sstride + r * 2], (uint8_t)sum,
			       (size_t)(width - 2 * r));
		}
		return ximage;
	}
	if (width < r * 2) {
		// Similarly, for width smaller than kernel
		for (int y = 0; y < r * 2; y++) {
			for (int x = 0; x < swidth; x++) {
				double sum = sum_kernel_normalized(kernel, d - x - 1,
				                                   d - y - 1, width, d) *
				             255.0;
				data[y * sstride + x] = (uint8_t)sum;
				data[(sheight - y - 1) * sstride + x] = (uint8_t)sum;
			}
		}
		for (int x = 0; x < swidth; x++) {
			double sum =
			    sum_kernel_normalized(kernel, d - x - 1, 0, width, d) * 255.0;
			for (int y = r * 2; y < height; y++) {
				data[y * sstride + x] = (uint8_t)sum;
			}
		}
		return ximage;
	}

	// Implies: width >= r * 2 && height >= r * 2

	// Fill part 3
	for (int y = r; y < height + r; y++) {
		memset(data + sstride * y + r, (uint8_t)(255 * opacity), (size_t)width);
	}

	// Part 1
	for (int y = 0; y < r * 2; y++) {
		for (int x = 0; x < r * 2; x++) {
			double tmpsum = shadow_sum[y * d + x] * opacity * 255.0;
			data[y * sstride + x] = (uint8_t)tmpsum;
			data[(sheight - y - 1) * sstride + x] = (uint8_t)tmpsum;
			data[(sheight - y - 1) * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
			data[y * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
		}
	}

	// Part 2, top/bottom
	for (int y = 0; y < r * 2; y++) {
		double tmpsum = shadow_sum[d * y + d - 1] * opacity * 255.0;
		memset(&data[y * sstride + r * 2], (uint8_t)tmpsum, (size_t)(width - r * 2));
		memset(&data[(sheight - y - 1) * sstride + r * 2], (uint8_t)tmpsum,
		       (size_t)(width - r * 2));
	}

	// Part 2, left/right
	for (int x = 0; x < r * 2; x++) {
		double tmpsum = shadow_sum[d * (d - 1) + x] * opacity * 255.0;
		for (int y = r * 2; y < height; y++) {
			data[y * sstride + x] = (uint8_t)tmpsum;
			data[y * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
		}
	}

	return ximage;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
bool build_shadow(xcb_connection_t *c, xcb_drawable_t d, double opacity, const int width,
                  const int height, const conv *kernel, xcb_render_picture_t shadow_pixel,
                  xcb_pixmap_t *pixmap, xcb_render_picture_t *pict) {
	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = XCB_NONE, shadow_pixmap_argb = XCB_NONE;
	xcb_render_picture_t shadow_picture = XCB_NONE, shadow_picture_argb = XCB_NONE;
	xcb_gcontext_t gc = XCB_NONE;

	shadow_image = make_shadow(c, kernel, opacity, width, height);
	if (!shadow_image) {
		log_error("Failed to make shadow");
		return false;
	}

	shadow_pixmap = x_create_pixmap(c, 8, d, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(c, 32, d, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("Failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    c, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    c, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
	if (!shadow_picture || !shadow_picture_argb) {
		goto shadow_picture_err;
	}

	gc = x_new_id(c);
	xcb_create_gc(c, gc, shadow_pixmap, 0, NULL);

	// We need to make room for protocol metadata in the request. The metadata should
	// be 24 bytes plus padding, let's be generous and give it 1kb
	auto maximum_image_size = xcb_get_maximum_request_length(c) * 4 - 1024;
	auto maximum_row =
	    to_u16_checked(clamp(maximum_image_size / shadow_image->stride, 0, UINT16_MAX));
	if (maximum_row <= 0) {
		// TODO(yshui) Upload image with XShm
		log_error("X server request size limit is too restrictive, or the shadow "
		          "image is too wide for us to send a single row of the shadow "
		          "image. Shadow size: %dx%d",
		          width, height);
		goto shadow_picture_err;
	}

	for (uint32_t row = 0; row < shadow_image->height; row += maximum_row) {
		auto batch_height = maximum_row;
		if (batch_height > shadow_image->height - row) {
			batch_height = to_u16_checked(shadow_image->height - row);
		}

		uint32_t offset = row * shadow_image->stride / sizeof(*shadow_image->data);
		xcb_put_image(c, (uint8_t)shadow_image->format, shadow_pixmap, gc,
		              shadow_image->width, batch_height, 0, to_i16_checked(row),
		              0, shadow_image->depth, shadow_image->stride * batch_height,
		              shadow_image->data + offset);
	}

	xcb_render_composite(c, XCB_RENDER_PICT_OP_SRC, shadow_pixel, shadow_picture,
	                     shadow_picture_argb, 0, 0, 0, 0, 0, 0, shadow_image->width,
	                     shadow_image->height);

	*pixmap = shadow_pixmap_argb;
	*pict = shadow_picture_argb;

	xcb_free_gc(c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(c, shadow_pixmap);
	xcb_render_free_picture(c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image) {
		xcb_image_destroy(shadow_image);
	}
	if (shadow_pixmap) {
		xcb_free_pixmap(c, shadow_pixmap);
	}
	if (shadow_pixmap_argb) {
		xcb_free_pixmap(c, shadow_pixmap_argb);
	}
	if (shadow_picture) {
		xcb_render_free_picture(c, shadow_picture);
	}
	if (shadow_picture_argb) {
		xcb_render_free_picture(c, shadow_picture_argb);
	}
	if (gc) {
		xcb_free_gc(c, gc);
	}

	return false;
}

void *
default_backend_render_shadow(backend_t *backend_data, int width, int height,
                              const conv *kernel, double r, double g, double b, double a) {
	xcb_pixmap_t shadow_pixel = solid_picture(backend_data->c, backend_data->root,
	                                          true, 1, r, g, b),
	             shadow = XCB_NONE;
	xcb_render_picture_t pict = XCB_NONE;

	if (!build_shadow(backend_data->c, backend_data->root, a, width, height, kernel,
	                  shadow_pixel, &shadow, &pict)) {
		return NULL;
	}

	auto visual = x_get_visual_for_standard(backend_data->c, XCB_PICT_STANDARD_ARGB_32);
	void *ret = backend_data->ops->bind_pixmap(
	    backend_data, shadow, x_get_visual_info(backend_data->c, visual), true);
	xcb_render_free_picture(backend_data->c, pict);
	return ret;
}

static struct conv **generate_box_blur_kernel(struct box_blur_args *args, int *kernel_count) {
	int r = args->size * 2 + 1;
	assert(r > 0);
	auto ret = ccalloc(2, struct conv *);
	ret[0] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[1] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[0]->w = r;
	ret[0]->h = 1;
	ret[1]->w = 1;
	ret[1]->h = r;
	for (int i = 0; i < r; i++) {
		ret[0]->data[i] = 1;
		ret[1]->data[i] = 1;
	}
	*kernel_count = 2;
	return ret;
}

static struct conv **
generate_gaussian_blur_kernel(struct gaussian_blur_args *args, int *kernel_count) {
	int r = args->size * 2 + 1;
	assert(r > 0);
	auto ret = ccalloc(2, struct conv *);
	ret[0] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[1] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[0]->w = r;
	ret[0]->h = 1;
	ret[1]->w = 1;
	ret[1]->h = r;
	for (int i = 0; i <= args->size; i++) {
		ret[0]->data[i] = ret[0]->data[r - i - 1] =
		    1.0 / (sqrt(2.0 * M_PI) * args->deviation) *
		    exp(-(args->size - i) * (args->size - i) /
		        (2 * args->deviation * args->deviation));
		ret[1]->data[i] = ret[1]->data[r - i - 1] = ret[0]->data[i];
	}
	*kernel_count = 2;
	return ret;
}

/// Generate blur kernels for gaussian and box blur methods. Generated kernel is not
/// normalized, and the center element will always be 1.
struct conv **generate_blur_kernel(enum blur_method method, void *args, int *kernel_count) {
	switch (method) {
	case BLUR_METHOD_BOX: return generate_box_blur_kernel(args, kernel_count);
	case BLUR_METHOD_GAUSSIAN:
		return generate_gaussian_blur_kernel(args, kernel_count);
	default: break;
	}
	return NULL;
}

/// Generate kernel parameters for dual-kawase blur method. Falls back on approximating
/// standard gauss radius if strength is zero or below.
struct dual_kawase_params *generate_dual_kawase_params(void *args) {
	struct dual_kawase_blur_args *blur_args = args;
	static const struct {
		int iterations;        /// Number of down- and upsample iterations
		float offset;          /// Sample offset in half-pixels
		int min_radius;        /// Approximate gauss-blur with at least this
		                       /// radius and std-deviation
	} strength_levels[20] = {
	    {.iterations = 1, .offset = 1.25f, .min_radius = 1},          // LVL  1
	    {.iterations = 1, .offset = 2.25f, .min_radius = 6},          // LVL  2
	    {.iterations = 2, .offset = 2.00f, .min_radius = 11},         // LVL  3
	    {.iterations = 2, .offset = 3.00f, .min_radius = 17},         // LVL  4
	    {.iterations = 2, .offset = 4.25f, .min_radius = 24},         // LVL  5
	    {.iterations = 3, .offset = 2.50f, .min_radius = 32},         // LVL  6
	    {.iterations = 3, .offset = 3.25f, .min_radius = 40},         // LVL  7
	    {.iterations = 3, .offset = 4.25f, .min_radius = 51},         // LVL  8
	    {.iterations = 3, .offset = 5.50f, .min_radius = 67},         // LVL  9
	    {.iterations = 4, .offset = 3.25f, .min_radius = 83},         // LVL 10
	    {.iterations = 4, .offset = 4.00f, .min_radius = 101},        // LVL 11
	    {.iterations = 4, .offset = 5.00f, .min_radius = 123},        // LVL 12
	    {.iterations = 4, .offset = 6.00f, .min_radius = 148},        // LVL 13
	    {.iterations = 4, .offset = 7.25f, .min_radius = 178},        // LVL 14
	    {.iterations = 4, .offset = 8.25f, .min_radius = 208},        // LVL 15
	    {.iterations = 5, .offset = 4.50f, .min_radius = 236},        // LVL 16
	    {.iterations = 5, .offset = 5.25f, .min_radius = 269},        // LVL 17
	    {.iterations = 5, .offset = 6.25f, .min_radius = 309},        // LVL 18
	    {.iterations = 5, .offset = 7.25f, .min_radius = 357},        // LVL 19
	    {.iterations = 5, .offset = 8.50f, .min_radius = 417},        // LVL 20
	};

	auto params = ccalloc(1, struct dual_kawase_params);
	params->iterations = 0;
	params->offset = 1.0f;

	if (blur_args->strength <= 0 && blur_args->size) {
		// find highest level that approximates blur-strength with the selected
		// gaussian blur-radius
		int lvl = 1;
		while (strength_levels[lvl - 1].min_radius < blur_args->size && lvl < 20) {
			++lvl;
		}
		blur_args->strength = lvl;
	}
	if (blur_args->strength <= 0) {
		// default value
		blur_args->strength = 5;
	}

	assert(blur_args->strength > 0 && blur_args->strength <= 20);
	params->iterations = strength_levels[blur_args->strength - 1].iterations;
	params->offset = strength_levels[blur_args->strength - 1].offset;

	// Expand sample area to cover the smallest texture / highest selected iteration:
	// - Smallest texture dimensions are halved `iterations`-times
	// - Upsample needs pixels two-times `offset` away from the border
	// - Plus one for interpolation differences
	params->expand = (1 << params->iterations) * 2 * (int)ceil(params->offset) + 1;

	return params;
}

void *default_clone_image(backend_t *base attr_unused, const void *image_data,
                          const region_t *reg_visible attr_unused) {
	auto new_img = ccalloc(1, struct backend_image);
	*new_img = *(struct backend_image *)image_data;
	new_img->inner->refcount++;
	return new_img;
}

bool default_set_image_property(backend_t *base attr_unused, enum image_properties op,
                                void *image_data, void *arg) {
	struct backend_image *tex = image_data;
	int *iargs = arg;
	bool *bargs = arg;
	double *dargs = arg;
	switch (op) {
	case IMAGE_PROPERTY_INVERTED: tex->color_inverted = bargs[0]; break;
	case IMAGE_PROPERTY_DIM_LEVEL: tex->dim = dargs[0]; break;
	case IMAGE_PROPERTY_OPACITY: tex->opacity = dargs[0]; break;
	case IMAGE_PROPERTY_EFFECTIVE_SIZE:
		// texture is already set to repeat, so nothing else we need to do
		tex->ewidth = iargs[0];
		tex->eheight = iargs[1];
		break;
	case IMAGE_PROPERTY_CORNER_RADIUS: tex->corner_radius = dargs[0]; break;
	case IMAGE_PROPERTY_MAX_BRIGHTNESS: tex->max_brightness = dargs[0]; break;
	}

	return true;
}

bool default_is_image_transparent(backend_t *base attr_unused, void *image_data) {
	struct backend_image *img = image_data;
	return img->opacity < 1 || img->inner->has_alpha;
}

struct backend_image *default_new_backend_image(int w, int h) {
	auto ret = ccalloc(1, struct backend_image);
	ret->opacity = 1;
	ret->dim = 0;
	ret->max_brightness = 1;
	ret->eheight = h;
	ret->ewidth = w;
	ret->color_inverted = false;
	return ret;
}

void init_backend_base(struct backend_base *base, session_t *ps) {
	base->c = ps->c;
	base->loop = ps->loop;
	base->root = ps->root;
	base->busy = false;
	base->ops = NULL;
}
