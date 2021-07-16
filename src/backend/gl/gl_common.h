// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdbool.h>
#include <string.h>

#include "backend/backend.h"
#include "log.h"
#include "region.h"

#define CASESTRRET(s)                                                                    \
	case s: return #s

// Program and uniforms for window shader
typedef struct {
	GLuint prog;
	GLint unifm_opacity;
	GLint unifm_invert_color;
	GLint unifm_tex;
	GLint unifm_dim;
	GLint unifm_brightness;
	GLint unifm_max_brightness;
	GLint unifm_corner_radius;
} gl_win_shader_t;

// Program and uniforms for brightness shader
typedef struct {
	GLuint prog;
} gl_brightness_shader_t;

// Program and uniforms for blur shader
typedef struct {
	GLuint prog;
	GLint unifm_pixel_norm;
	GLint unifm_opacity;
	GLint orig_loc;
	GLint texorig_loc;
	GLint scale_loc;
} gl_blur_shader_t;

typedef struct {
	GLuint prog;
	GLint color_loc;
} gl_fill_shader_t;

/// @brief Wrapper of a binded GLX texture.
struct gl_texture {
	int refcount;
	bool has_alpha;
	GLuint texture;
	int width, height;
	bool y_inverted;

	// Textures for auxiliary uses.
	GLuint auxiliary_texture[2];
	void *user_data;
};

struct gl_data {
	backend_t base;
	// If we are using proprietary NVIDIA driver
	bool is_nvidia;
	// Height and width of the root window
	int height, width;
	gl_win_shader_t win_shader;
	gl_brightness_shader_t brightness_shader;
	gl_fill_shader_t fill_shader;
	GLuint back_texture, back_fbo;
	GLuint present_prog;

	/// Called when an gl_texture is decoupled from the texture it refers. Returns
	/// the decoupled user_data
	void *(*decouple_texture_user_data)(backend_t *base, void *user_data);

	/// Release the user data attached to a gl_texture
	void (*release_user_data)(backend_t *base, struct gl_texture *);

	struct log_target *logger;
};

typedef struct session session_t;

#define GL_PROG_MAIN_INIT                                                                \
	{ .prog = 0, .unifm_opacity = -1, .unifm_invert_color = -1, .unifm_tex = -1, }

GLuint gl_create_shader(GLenum shader_type, const char *shader_str);
GLuint gl_create_program(const GLuint *const shaders, int nshaders);
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str);

/**
 * @brief Render a region with texture data.
 */
void gl_compose(backend_t *, void *ptex, int dst_x, int dst_y, const region_t *reg_tgt,
                const region_t *reg_visible);

void gl_resize(struct gl_data *, int width, int height);

bool gl_init(struct gl_data *gd, session_t *);
void gl_deinit(struct gl_data *gd);

GLuint gl_new_texture(GLenum target);

bool gl_image_op(backend_t *base, enum image_operations op, void *image_data,
                 const region_t *reg_op, const region_t *reg_visible, void *arg);

void gl_release_image(backend_t *base, void *image_data);

void *gl_clone(backend_t *base, const void *image_data, const region_t *reg_visible);

bool gl_blur(backend_t *base, double opacity, void *, const region_t *reg_blur,
             const region_t *reg_visible);
void *gl_create_blur_context(backend_t *base, enum blur_method, void *args);
void gl_destroy_blur_context(backend_t *base, void *ctx);
void gl_get_blur_size(void *blur_context, int *width, int *height);

void gl_fill(backend_t *base, struct color, const region_t *clip);

void gl_present(backend_t *base, const region_t *);
bool gl_read_pixel(backend_t *base, void *image_data, int x, int y, struct color *output);

static inline void gl_delete_texture(GLuint texture) {
	glDeleteTextures(1, &texture);
}

/**
 * Get a textual representation of an OpenGL error.
 */
static inline const char *gl_get_err_str(GLenum err) {
	switch (err) {
		CASESTRRET(GL_NO_ERROR);
		CASESTRRET(GL_INVALID_ENUM);
		CASESTRRET(GL_INVALID_VALUE);
		CASESTRRET(GL_INVALID_OPERATION);
		CASESTRRET(GL_INVALID_FRAMEBUFFER_OPERATION);
		CASESTRRET(GL_OUT_OF_MEMORY);
		CASESTRRET(GL_STACK_UNDERFLOW);
		CASESTRRET(GL_STACK_OVERFLOW);
		CASESTRRET(GL_FRAMEBUFFER_UNDEFINED);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER);
		CASESTRRET(GL_FRAMEBUFFER_UNSUPPORTED);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS);
	}
	return NULL;
}

/**
 * Check for GLX error.
 *
 * http://blog.nobel-joergensen.com/2013/01/29/debugging-opengl-using-glgeterror/
 */
static inline void gl_check_err_(const char *func, int line) {
	GLenum err = GL_NO_ERROR;

	while (GL_NO_ERROR != (err = glGetError())) {
		const char *errtext = gl_get_err_str(err);
		if (errtext) {
			log_printf(tls_logger, LOG_LEVEL_ERROR, func,
			           "GLX error at line %d: %s", line, errtext);
		} else {
			log_printf(tls_logger, LOG_LEVEL_ERROR, func,
			           "GLX error at line %d: %d", line, err);
		}
	}
}

static inline void gl_clear_err(void) {
	while (glGetError() != GL_NO_ERROR)
		;
}

#define gl_check_err() gl_check_err_(__func__, __LINE__)

/**
 * Check for GL framebuffer completeness.
 */
static inline bool gl_check_fb_complete_(const char *func, int line, GLenum fb) {
	GLenum status = glCheckFramebufferStatus(fb);

	if (status == GL_FRAMEBUFFER_COMPLETE) {
		return true;
	}

	const char *stattext = gl_get_err_str(status);
	if (stattext) {
		log_printf(tls_logger, LOG_LEVEL_ERROR, func,
		           "Framebuffer attachment failed at line %d: %s", line, stattext);
	} else {
		log_printf(tls_logger, LOG_LEVEL_ERROR, func,
		           "Framebuffer attachment failed at line %d: %d", line, status);
	}

	return false;
}

#define gl_check_fb_complete(fb) gl_check_fb_complete_(__func__, __LINE__, (fb))

/**
 * Check if a GLX extension exists.
 */
static inline bool gl_has_extension(const char *ext) {
	int nexts = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &nexts);
	for (int i = 0; i < nexts || !nexts; i++) {
		const char *exti = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (exti == NULL) {
			break;
		}
		if (strcmp(ext, exti) == 0) {
			return true;
		}
	}
	gl_clear_err();
	log_info("Missing GL extension %s.", ext);
	return false;
}
