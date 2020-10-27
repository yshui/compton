// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include <test.h>

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

const char *xdg_config_home(void) {
	char *xdgh = getenv("XDG_CONFIG_HOME");
	char *home = getenv("HOME");
	const char *default_dir = "/.config";

	if (!xdgh) {
		if (!home) {
			return NULL;
		}

		xdgh = cvalloc(strlen(home) + strlen(default_dir) + 1);

		strcpy(xdgh, home);
		strcat(xdgh, default_dir);
	} else {
		xdgh = strdup(xdgh);
	}

	return xdgh;
}

char **xdg_config_dirs(void) {
	char *xdgd = getenv("XDG_CONFIG_DIRS");
	size_t count = 0;

	if (!xdgd) {
		xdgd = "/etc/xdg";
	}

	for (int i = 0; xdgd[i]; i++) {
		if (xdgd[i] == ':') {
			count++;
		}
	}

	// Store the string and the result pointers together so they can be
	// freed together
	char **dir_list = cvalloc(sizeof(char *) * (count + 2) + strlen(xdgd) + 1);
	auto dirs = strcpy((char *)dir_list + sizeof(char *) * (count + 2), xdgd);
	auto path = dirs;

	for (size_t i = 0; i < count; i++) {
		dir_list[i] = path;
		path = strchr(path, ':');
		*path = '\0';
		path++;
	}
	dir_list[count] = path;

	size_t fill = 0;
	for (size_t i = 0; i <= count; i++) {
		if (dir_list[i][0] == '/') {
			dir_list[fill] = dir_list[i];
			fill++;
		}
	}

	dir_list[fill] = NULL;

	return dir_list;
}

TEST_CASE(xdg_config_dirs) {
	auto old_var = getenv("XDG_CONFIG_DIRS");
	if (old_var) {
		old_var = strdup(old_var);
	}
	unsetenv("XDG_CONFIG_DIRS");

	auto result = xdg_config_dirs();
	TEST_STREQUAL(result[0], "/etc/xdg");
	TEST_EQUAL(result[1], NULL);
	free(result);

	setenv("XDG_CONFIG_DIRS", ".:.:/etc/xdg:.:/:", 1);
	result = xdg_config_dirs();
	TEST_STREQUAL(result[0], "/etc/xdg");
	TEST_STREQUAL(result[1], "/");
	TEST_EQUAL(result[2], NULL);
	free(result);

	setenv("XDG_CONFIG_DIRS", ":", 1);
	result = xdg_config_dirs();
	TEST_EQUAL(result[0], NULL);
	free(result);

	if (old_var) {
		setenv("XDG_CONFIG_DIRS", old_var, 1);
		free(old_var);
	}
}

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

/// Search for shader file under a base directory
FILE *open_shader_file_at(const char *base, const char *file, char **out_path) {
	// TODO(tryone144): Should we bother supporting the old "compton" name?
	static const char *shader_paths[] = {"/picom/shaders/", "/compton/shaders/"};
	for (size_t i = 0; i < ARR_SIZE(shader_paths); i++) {
		char *path = mstrjoin(base, shader_paths[i]);
		mstrextend(&path, file);
		FILE *ret = fopen(path, "r");
		if (ret && out_path) {
			*out_path = path;
		} else {
			free(path);
		}
		if (ret) {
			if (strstr(shader_paths[i], "compton")) {
				log_warn("This compositor has been renamed to \"picom\", "
				         "the old config file paths is deprecated. "
				         "Please replace the \"compton\"s in the path "
				         "with \"picom\"");
			}
			return ret;
		}
	}
	return NULL;
}

/**
 * Get a file stream of the shader file to read.
 *
 * Follows the XDG specification to search for the shader file in configuration locations.
 */
FILE *open_shader_file(const char *spath, const char *include_dir, char **ppath) {
	if (!spath || strlen(spath) == 0) {
		return NULL;
	}

	FILE *ret = NULL;

	// Filename is absolute path, so try to load from there
	if (spath[0] == '/') {
		ret = fopen(spath, "r");
		if (ret && ppath) {
			*ppath = strdup(spath);
		}
		return ret;
	}
	if (spath[0] == '.') {
		auto path = realpath(spath, NULL);
		ret = fopen(path, "r");
		if (ret && ppath) {
			*ppath = path;
		} else {
			free(path);
		}
		return ret;
	}

	// First try to load shader file from the include directory (i.e. relative to the
	// config file)
	if (include_dir && strlen(include_dir)) {
		auto path = mstrjoin(include_dir, "/");
		mstrextend(&path, spath);
		ret = fopen(path, "r");
		if (ret && ppath) {
			*ppath = path;
		} else {
			free(path);
		}
		if (ret) {
			return ret;
		}
	}

	// Fall back to shader file in user config directory
	auto config_home = xdg_config_home();
	ret = open_shader_file_at(config_home, spath, ppath);
	free((void *)config_home);
	if (ret) {
		return ret;
	}

	// Fall back to shader file in system config directory
	auto config_dirs = xdg_config_dirs();
	for (int i = 0; config_dirs[i]; i++) {
		ret = open_shader_file_at(config_dirs[i], spath, ppath);
		if (ret) {
			free(config_dirs);
			return ret;
		}
	}
	free(config_dirs);

	return ret;
}

/**
 * Parse a window shader preset or load from file.
 */
const struct custom_shader *
parse_custom_shader(const char *src, struct custom_shader **shaders, const char *include_dir) {
	if (!shaders || !src) {
		return NULL;
	}

	// Skip over spaces
	const char *startptr = src;
	while (*startptr && isspace(*startptr)) {
		++startptr;
	}
	const char *endptr = startptr;

	// Check if shader is default preset
	bool is_default = false;
	if (mstrncmp("default", startptr) == 0) {
		// Skip over first word
		endptr += strlen("default");
		// Skip over trailing spaces
		while (*endptr && isspace(*endptr)) {
			++endptr;
		}
		is_default = !*endptr;
	}

	if (is_default) {
		return &custom_shader_default;
	}

	// Try to open shader file in common locations
	char *path = NULL;
	FILE *f = open_shader_file(src, include_dir, &path);
	if (!f) {
		free(path);
		log_error("Failed to open custom shader file \"%s\".", src);
		return NULL;
	}
	log_debug("Load custom fragment shader \"%s\" at %s", src, path);

	// Check if shader has already been loaded. If so, return the cached
	// instance.
	struct custom_shader *old_shader;
	HASH_FIND_STR(*shaders, path, old_shader);

	if (old_shader) {
		fclose(f);
		free(path);
		return old_shader;
	}

	// Create new shader and load from file
	struct stat statbuf;
	int ret = fstat(fileno(f), &statbuf);
	if (ret < 0) {
		fclose(f);
		log_error_errno("Failed to retrieve information about custom shader "
		                "\"%s\" at \"%s\"",
		                src, path);
		free(path);
		return NULL;
	}
	auto num_bytes = (size_t)statbuf.st_size;

	auto shader = ccalloc(1, struct custom_shader);
	shader->path = path;
	shader->source = ccalloc(num_bytes + 1, char);
	if ((fread(shader->source, sizeof(char), num_bytes, f) < num_bytes) && ferror(f)) {
		fclose(f);
		log_error("Failed to read custom shader \"%s\" at %s.", src, path);
		free(shader->path);
		free(shader->source);
		free(shader);
		return NULL;
	}

	// Add newly created shader with unique id.
	uint32_t next_id = CUSTOM_SHADER_CUSTOM_START;
	struct custom_shader *tmp;
	HASH_ITER(hh, *shaders, old_shader, tmp) {
		if (old_shader->id >= next_id) {
			next_id = old_shader->id + 1;
		}
	}
	shader->id = next_id;
	HASH_ADD_KEYPTR(hh, *shaders, shader->path, strlen(shader->path), shader);

	fclose(f);
	return shader;
}

/**
 * Parse a list of window shader rules.
 */
bool parse_rule_window_shader(c2_lptr_t **res, struct custom_shader **shaders,
                              const char *src, const char *include_dir) {
	if (!shaders || !src) {
		return false;
	}

	// Find custom shader terminator
	const char *endptr = strchr(src, ':');
	if (!endptr) {
		log_error("Custom shader terminator not found: %s", src);
		return false;
	}

	// Parse and create custom shader
	auto shader_source = strdup(src);
	if (shader_source) {
		auto source_end = strchr(shader_source, ':');
		*source_end = '\0';
	}

	auto shader = parse_custom_shader(shader_source, shaders, include_dir);
	free(shader_source);
	if (!shader) {
		log_error("Custom shader \"%s\" invalid", src);
		return false;
	}

	// Parse pattern
	return c2_parse(res, ++endptr, (void *)shader);
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
	    .custom_shaders = NULL,
	    .window_shader_fg = NULL,
	    .window_shader_fg_rules = NULL,
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
