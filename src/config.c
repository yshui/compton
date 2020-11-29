// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "string_utils.h"
#include "types.h"
#include "utils.h"
#include "win.h"

#include "config.h"

/**
 * Parse a long number.
 */
bool parse_long(const char *s, long *dest) {
	const char *endptr = NULL;
	long val = strtol(s, (char **)&endptr, 0);
	if (!endptr || endptr == s) {
		log_error("Invalid number: %s", s);
		return false;
	}
	while (isspace(*endptr))
		++endptr;
	if (*endptr) {
		log_error("Trailing characters: %s", s);
		return false;
	}
	*dest = val;
	return true;
}

/**
 * Parse an int  number.
 */
bool parse_int(const char *s, int *dest) {
	long val;
	if (!parse_long(s, &val)) {
		return false;
	}
	if (val > INT_MAX || val < INT_MIN) {
		log_error("Number exceeded int limits: %ld", val);
		return false;
	}
	*dest = (int)val;
	return true;
}

/**
 * Parse a floating-point number in from a string,
 * also strips the trailing space and comma after the number.
 *
 * @param[in]  src  string to parse
 * @param[out] dest return the number parsed from the string
 * @return          pointer to the last character parsed
 */
const char *parse_readnum(const char *src, double *dest) {
	const char *pc = NULL;
	double val = strtod_simple(src, &pc);
	if (!pc || pc == src) {
		log_error("No number found: %s", src);
		return src;
	}
	while (*pc && (isspace(*pc) || *pc == ',')) {
		++pc;
	}
	*dest = val;
	return pc;
}

enum blur_method parse_blur_method(const char *src) {
	if (strcmp(src, "kernel") == 0) {
		return BLUR_METHOD_KERNEL;
	} else if (strcmp(src, "box") == 0) {
		return BLUR_METHOD_BOX;
	} else if (strcmp(src, "gaussian") == 0) {
		return BLUR_METHOD_GAUSSIAN;
	} else if (strcmp(src, "dual_kawase") == 0) {
		return BLUR_METHOD_DUAL_KAWASE;
	} else if (strcmp(src, "kawase") == 0) {
		log_warn("Blur method 'kawase' has been renamed to 'dual_kawase'. "
		         "Interpreted as 'dual_kawase', but this will stop working "
		         "soon.");
		return BLUR_METHOD_DUAL_KAWASE;
	} else if (strcmp(src, "none") == 0) {
		return BLUR_METHOD_NONE;
	}
	return BLUR_METHOD_INVALID;
}

/**
 * Parse a matrix.
 *
 * @param[in]  src    the blur kernel string
 * @param[out] endptr return where the end of kernel is in the string
 * @param[out] hasneg whether the kernel has negative values
 */
conv *parse_blur_kern(const char *src, const char **endptr, bool *hasneg) {
	int width = 0, height = 0;
	*hasneg = false;

	const char *pc = NULL;

	// Get matrix width and height
	double val = 0.0;
	if (src == (pc = parse_readnum(src, &val)))
		goto err1;
	src = pc;
	width = (int)val;
	if (src == (pc = parse_readnum(src, &val)))
		goto err1;
	src = pc;
	height = (int)val;

	// Validate matrix width and height
	if (width <= 0 || height <= 0) {
		log_error("Blue kernel width/height can't be negative.");
		goto err1;
	}
	if (!(width % 2 && height % 2)) {
		log_error("Blur kernel width/height must be odd.");
		goto err1;
	}
	if (width > 16 || height > 16)
		log_warn("Blur kernel width/height too large, may slow down"
		         "rendering, and/or consume lots of memory");

	// Allocate memory
	conv *matrix = cvalloc(sizeof(conv) + (size_t)(width * height) * sizeof(double));

	// Read elements
	int skip = height / 2 * width + width / 2;
	for (int i = 0; i < width * height; ++i) {
		// Ignore the center element
		if (i == skip) {
			matrix->data[i] = 1;
			continue;
		}
		if (src == (pc = parse_readnum(src, &val))) {
			goto err2;
		}
		src = pc;
		if (val < 0) {
			*hasneg = true;
		}
		matrix->data[i] = val;
	}

	// Detect trailing characters
	for (; *pc && *pc != ';'; pc++) {
		if (!isspace(*pc) && *pc != ',') {
			// TODO(yshui) isspace is locale aware, be careful
			log_error("Trailing characters in blur kernel string.");
			goto err2;
		}
	}

	// Jump over spaces after ';'
	if (*pc == ';') {
		pc++;
		while (*pc && isspace(*pc)) {
			++pc;
		}
	}

	// Require an end of string if endptr is not provided, otherwise
	// copy end pointer to endptr
	if (endptr) {
		*endptr = pc;
	} else if (*pc) {
		log_error("Only one blur kernel expected.");
		goto err2;
	}

	// Fill in width and height
	matrix->w = width;
	matrix->h = height;
	return matrix;

err2:
	free(matrix);
err1:
	return NULL;
}

/**
 * Parse a list of convolution kernels.
 *
 * @param[in]  src    string to parse
 * @param[out] hasneg whether any of the kernels have negative values
 * @return            the kernels
 */
struct conv **parse_blur_kern_lst(const char *src, bool *hasneg, int *count) {
	// TODO(yshui) just return a predefined kernels, not parse predefined strings...
	static const struct {
		const char *name;
		const char *kern_str;
	} CONV_KERN_PREDEF[] = {
	    {"3x3box", "3,3,1,1,1,1,1,1,1,1,"},
	    {"5x5box", "5,5,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"},
	    {"7x7box", "7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
	               "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"},
	    {"3x3gaussian", "3,3,0.243117,0.493069,0.243117,0.493069,0.493069,0.243117,0."
	                    "493069,0.243117,"},
	    {"5x5gaussian", "5,5,0.003493,0.029143,0.059106,0.029143,0.003493,0.029143,0."
	                    "243117,0.493069,0.243117,0.029143,0.059106,0.493069,0."
	                    "493069,0.059106,0.029143,0.243117,0.493069,0.243117,0."
	                    "029143,0.003493,0.029143,0.059106,0.029143,0.003493,"},
	    {"7x7gaussian", "7,7,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0."
	                    "000003,0.000102,0.003493,0.029143,0.059106,0.029143,0."
	                    "003493,0.000102,0.000849,0.029143,0.243117,0.493069,0."
	                    "243117,0.029143,0.000849,0.001723,0.059106,0.493069,0."
	                    "493069,0.059106,0.001723,0.000849,0.029143,0.243117,0."
	                    "493069,0.243117,0.029143,0.000849,0.000102,0.003493,0."
	                    "029143,0.059106,0.029143,0.003493,0.000102,0.000003,0."
	                    "000102,0.000849,0.001723,0.000849,0.000102,0.000003,"},
	    {"9x9gaussian",
	     "9,9,0.000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0."
	     "000000,0.000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0."
	     "000102,0.000003,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0."
	     "029143,0.003493,0.000102,0.000001,0.000006,0.000849,0.029143,0.243117,0."
	     "493069,0.243117,0.029143,0.000849,0.000006,0.000012,0.001723,0.059106,0."
	     "493069,0.493069,0.059106,0.001723,0.000012,0.000006,0.000849,0.029143,0."
	     "243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000001,0.000102,0."
	     "003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0."
	     "000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000000,0."
	     "000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0.000000,0."
	     "000000,"},
	    {"11x11gaussian",
	     "11,11,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000001,0."
	     "000006,0.000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0."
	     "000000,0.000000,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0."
	     "029143,0.003493,0.000102,0.000001,0.000000,0.000000,0.000006,0.000849,0."
	     "029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000000,0."
	     "000000,0.000012,0.001723,0.059106,0.493069,0.493069,0.059106,0.001723,0."
	     "000012,0.000000,0.000000,0.000006,0.000849,0.029143,0.243117,0.493069,0."
	     "243117,0.029143,0.000849,0.000006,0.000000,0.000000,0.000001,0.000102,0."
	     "003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0."
	     "000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0."
	     "000003,0.000000,0.000000,0.000000,0.000000,0.000000,0.000001,0.000006,0."
	     "000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,"},
	};

	*count = 0;
	*hasneg = false;
	for (unsigned int i = 0;
	     i < sizeof(CONV_KERN_PREDEF) / sizeof(CONV_KERN_PREDEF[0]); ++i) {
		if (!strcmp(CONV_KERN_PREDEF[i].name, src))
			return parse_blur_kern_lst(CONV_KERN_PREDEF[i].kern_str, hasneg, count);
	}

	int nkernels = 1;
	for (int i = 0; src[i]; i++) {
		if (src[i] == ';') {
			nkernels++;
		}
	}

	struct conv **ret = ccalloc(nkernels, struct conv *);

	int i = 0;
	const char *pc = src;

	// Continue parsing until the end of source string
	i = 0;
	while (pc && *pc) {
		bool tmp_hasneg;
		assert(i < nkernels);
		ret[i] = parse_blur_kern(pc, &pc, &tmp_hasneg);
		if (!ret[i]) {
			for (int j = 0; j < i; j++) {
				free(ret[j]);
			}
			free(ret);
			return NULL;
		}
		i++;
		*hasneg |= tmp_hasneg;
	}

	if (i > 1) {
		log_warn("You are seeing this message because you are using "
		         "multipass blur. Please report an issue to us so we know "
		         "multipass blur is actually been used. Otherwise it might be "
		         "removed in future releases");
	}

	*count = i;

	return ret;
}

/**
 * Parse a X geometry.
 *
 * ps->root_width and ps->root_height must be valid
 */
bool parse_geometry(session_t *ps, const char *src, region_t *dest) {
	pixman_region32_clear(dest);
	if (!src) {
		return true;
	}
	if (!ps->root_width || !ps->root_height) {
		return true;
	}

	long x = 0, y = 0;
	long width = ps->root_width, height = ps->root_height;
	long val = 0L;
	char *endptr = NULL;

	src = skip_space(src);
	if (!*src) {
		goto parse_geometry_end;
	}

	// Parse width
	// Must be base 10, because "0x0..." may appear
	if (*src != '+' && *src != '-') {
		val = strtol(src, &endptr, 10);
		assert(endptr);
		if (src != endptr) {
			if (val < 0) {
				log_error("Invalid width: %s", src);
				return false;
			}
			width = val;
			src = endptr;
		}
		src = skip_space(src);
	}

	// Parse height
	if (*src == 'x') {
		++src;
		val = strtol(src, &endptr, 10);
		assert(endptr);
		if (src != endptr) {
			if (val < 0) {
				log_error("Invalid height: %s", src);
				return false;
			}
			height = val;
			src = endptr;
		}
		src = skip_space(src);
	}

	// Parse x
	if (*src == '+' || *src == '-') {
		val = strtol(src, &endptr, 10);
		if (endptr && src != endptr) {
			x = val;
			if (*src == '-') {
				x += ps->root_width - width;
			}
			src = endptr;
		}
		src = skip_space(src);
	}

	// Parse y
	if (*src == '+' || *src == '-') {
		val = strtol(src, &endptr, 10);
		if (endptr && src != endptr) {
			y = val;
			if (*src == '-') {
				y += ps->root_height - height;
			}
			src = endptr;
		}
		src = skip_space(src);
	}

	if (*src) {
		log_error("Trailing characters: %s", src);
		return false;
	}

parse_geometry_end:
	if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) {
		log_error("Geometry coordinates exceeded limits: %s", src);
		return false;
	}
	if (width > UINT_MAX || height > UINT_MAX) {
		// less than 0 is checked for earlier
		log_error("Geometry size exceeded limits: %s", src);
		return false;
	}
	pixman_region32_union_rect(dest, dest, (int)x, (int)y, (uint)width, (uint)height);
	return true;
}

/**
 * Parse a list of opacity rules.
 */
bool parse_rule_opacity(c2_lptr_t **res, const char *src) {
	// Find opacity value
	char *endptr = NULL;
	long val = strtol(src, &endptr, 0);
	if (!endptr || endptr == src) {
		log_error("No opacity specified: %s", src);
		return false;
	}
	if (val > 100 || val < 0) {
		log_error("Opacity %ld invalid: %s", val, src);
		return false;
	}

	// Skip over spaces
	while (*endptr && isspace(*endptr))
		++endptr;
	if (':' != *endptr) {
		log_error("Opacity terminator not found: %s", src);
		return false;
	}
	++endptr;

	// Parse pattern
	// I hope 1-100 is acceptable for (void *)
	return c2_parse(res, endptr, (void *)val);
}

/**
 * Add a pattern to a condition linked list.
 */
bool condlst_add(c2_lptr_t **pcondlst, const char *pattern) {
	if (!pattern)
		return false;

	if (!c2_parse(pcondlst, pattern, NULL))
		exit(1);

	return true;
}

void set_default_winopts(options_t *opt, win_option_mask_t *mask, bool shadow_enable,
                         bool fading_enable, bool blur_enable) {
	// Apply default wintype options.
	if (!mask[WINTYPE_DESKTOP].shadow) {
		// Desktop windows are always drawn without shadow by default.
		mask[WINTYPE_DESKTOP].shadow = true;
		opt->wintype_option[WINTYPE_DESKTOP].shadow = false;
	}

	// Focused/unfocused state only apply to a few window types, all other windows
	// are always considered focused.
	const wintype_t nofocus_type[] = {WINTYPE_UNKNOWN, WINTYPE_NORMAL, WINTYPE_UTILITY};
	for (unsigned long i = 0; i < ARR_SIZE(nofocus_type); i++) {
		if (!mask[nofocus_type[i]].focus) {
			mask[nofocus_type[i]].focus = true;
			opt->wintype_option[nofocus_type[i]].focus = false;
		}
	}
	for (unsigned long i = 0; i < NUM_WINTYPES; i++) {
		if (!mask[i].shadow) {
			mask[i].shadow = true;
			opt->wintype_option[i].shadow = shadow_enable;
		}
		if (!mask[i].fade) {
			mask[i].fade = true;
			opt->wintype_option[i].fade = fading_enable;
		}
		if (!mask[i].focus) {
			mask[i].focus = true;
			opt->wintype_option[i].focus = true;
		}
		if (!mask[i].blur_background) {
			mask[i].blur_background = true;
			opt->wintype_option[i].blur_background = blur_enable;
		}
		if (!mask[i].full_shadow) {
			mask[i].full_shadow = true;
			opt->wintype_option[i].full_shadow = false;
		}
		if (!mask[i].redir_ignore) {
			mask[i].redir_ignore = true;
			opt->wintype_option[i].redir_ignore = false;
		}
		if (!mask[i].opacity) {
			mask[i].opacity = true;
			// Opacity is not set to a concrete number here because the
			// opacity logic is complicated, and needs an "unset" state
			opt->wintype_option[i].opacity = NAN;
		}
	}
}

char *parse_config(options_t *opt, const char *config_file, bool *shadow_enable,
                   bool *fading_enable, bool *hasneg, win_option_mask_t *winopt_mask) {
	*opt = (struct options){
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
	    .use_damage = true,

	    .shadow_red = 0.0,
	    .shadow_green = 0.0,
	    .shadow_blue = 0.0,
	    .shadow_radius = 18,
	    .shadow_offset_x = -15,
	    .shadow_offset_y = -15,
	    .shadow_opacity = .75,
	    .shadow_blacklist = NULL,
	    .shadow_ignore_shaped = false,
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

	    .blur_method = BLUR_METHOD_NONE,
	    .blur_radius = 3,
	    .blur_deviation = 0.84089642,
	    .blur_strength = 5,
	    .blur_background_frame = false,
	    .blur_background_fixed = false,
	    .blur_background_blacklist = NULL,
	    .blur_kerns = NULL,
	    .blur_kernel_count = 0,
	    .inactive_blur = false,
	    .inactive_dim = 0.0,
	    .inactive_dim_fixed = false,
	    .invert_color_list = NULL,
	    .opacity_rules = NULL,
	    .max_brightness = 1.0,

	    .use_ewmh_active_win = false,
	    .focus_blacklist = NULL,
	    .detect_transient = false,
	    .detect_client_leader = false,
	    .no_ewmh_fullscreen = false,

	    .track_leader = false,
	};

	char *ret = NULL;
#ifdef CONFIG_LIBCONFIG
	ret = parse_config_libconfig(opt, config_file, shadow_enable, fading_enable,
	                             hasneg, winopt_mask);
#else
	(void)config_file;
	(void)shadow_enable;
	(void)fading_enable;
	(void)hasneg;
	(void)winopt_mask;
#endif
	return ret;
}
