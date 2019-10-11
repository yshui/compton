// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

/// Common functions and definitions for configuration parsing
/// Used for command line arguments and config files

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#ifdef CONFIG_LIBCONFIG
#include <libconfig.h>
#endif

#include "compiler.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "win_defs.h"
#include "types.h"

typedef struct session session_t;

/// @brief Possible backends of compton.
enum backend {
	BKEND_XRENDER,
	BKEND_GLX,
	BKEND_XR_GLX_HYBRID,
	BKEND_DUMMY,
	NUM_BKEND,
};

typedef struct win_option_mask {
	bool shadow : 1;
	bool fade : 1;
	bool focus : 1;
	bool full_shadow : 1;
	bool redir_ignore : 1;
	bool opacity : 1;
} win_option_mask_t;

typedef struct win_option {
	bool shadow;
	bool fade;
	bool focus;
	bool full_shadow;
	bool redir_ignore;
	double opacity;
} win_option_t;

enum blur_method {
	BLUR_METHOD_NONE = 0,
	BLUR_METHOD_KERNEL,
	BLUR_METHOD_BOX,
	BLUR_METHOD_GAUSSIAN,
	BLUR_METHOD_INVALID,
};

typedef struct _c2_lptr c2_lptr_t;

/// Structure representing all options.
typedef struct options {
	// === Debugging ===
	bool monitor_repaint;
	bool print_diagnostics;
	/// Render to a separate window instead of taking over the screen
	bool debug_mode;
	// === General ===
	/// Path to write PID to.
	char *write_pid_path;
	/// The backend in use.
	enum backend backend;
	/// Whether to sync X drawing with X Sync fence to avoid certain delay
	/// issues with GLX backend.
	bool xrender_sync_fence;
	/// Whether to avoid using stencil buffer under GLX backend. Might be
	/// unsafe.
	bool glx_no_stencil;
	/// Whether to avoid rebinding pixmap on window damage.
	bool glx_no_rebind_pixmap;
	/// Custom fragment shader for painting windows, as a string.
	char *glx_fshader_win_str;
	/// Whether to detect rounded corners.
	bool detect_rounded_corners;
	/// Force painting of window content with blending.
	bool force_win_blend;
	/// Resize damage for a specific number of pixels.
	int resize_damage;
	/// Whether to unredirect all windows if a full-screen opaque window
	/// is detected.
	bool unredir_if_possible;
	/// List of conditions of windows to ignore as a full-screen window
	/// when determining if a window could be unredirected.
	c2_lptr_t *unredir_if_possible_blacklist;
	/// Delay before unredirecting screen, in milliseconds.
	long unredir_if_possible_delay;
	/// Forced redirection setting through D-Bus.
	switch_t redirected_force;
	/// Whether to stop painting. Controlled through D-Bus.
	switch_t stoppaint_force;
	/// Whether to enable D-Bus support.
	bool dbus;
	/// Path to log file.
	char *logpath;
	/// Number of cycles to paint in benchmark mode. 0 for disabled.
	int benchmark;
	/// Window to constantly repaint in benchmark mode. 0 for full-screen.
	xcb_window_t benchmark_wid;
	/// A list of conditions of windows not to paint.
	c2_lptr_t *paint_blacklist;
	/// Whether to show all X errors.
	bool show_all_xerrors;
	/// Whether to avoid acquiring X Selection.
	bool no_x_selection;
	/// Window type option override.
	win_option_t wintype_option[NUM_WINTYPES];

	// === VSync & software optimization ===
	/// User-specified refresh rate.
	int refresh_rate;
	/// Whether to enable refresh-rate-based software optimization.
	bool sw_opti;
	/// VSync method to use;
	bool vsync;
	/// Whether to use glFinish() instead of glFlush() for (possibly) better
	/// VSync yet probably higher CPU usage.
	bool vsync_use_glfinish;
	/// Whether use damage information to help limit the area to paint
	bool use_damage;

	// === Shadow ===
	/// Red, green and blue tone of the shadow.
	double shadow_red, shadow_green, shadow_blue;
	int shadow_radius;
	int shadow_offset_x, shadow_offset_y;
	double shadow_opacity;
	/// argument string to shadow-exclude-reg option
	char *shadow_exclude_reg_str;
	/// Shadow blacklist. A linked list of conditions.
	c2_lptr_t *shadow_blacklist;
	/// Whether bounding-shaped window should be ignored.
	bool shadow_ignore_shaped;
	/// Whether to respect _COMPTON_SHADOW.
	bool respect_prop_shadow;
	/// Whether to crop shadow to the very Xinerama screen.
	bool xinerama_shadow_crop;

	// === Fading ===
	/// How much to fade in in a single fading step.
	double fade_in_step;
	/// How much to fade out in a single fading step.
	double fade_out_step;
	/// Fading time delta. In milliseconds.
	int fade_delta;
	/// Whether to disable fading on window open/close.
	bool no_fading_openclose;
	/// Whether to disable fading on ARGB managed destroyed windows.
	bool no_fading_destroyed_argb;
	/// Fading blacklist. A linked list of conditions.
	c2_lptr_t *fade_blacklist;

	// === Opacity ===
	/// Default opacity for inactive windows.
	/// 32-bit integer with the format of _NET_WM_OPACITY.
	double inactive_opacity;
	/// Default opacity for inactive windows.
	double active_opacity;
	/// Whether inactive_opacity overrides the opacity set by window
	/// attributes.
	bool inactive_opacity_override;
	/// Frame opacity. Relative to window opacity, also affects shadow
	/// opacity.
	double frame_opacity;
	/// Whether to detect _NET_WM_OPACITY on client windows. Used on window
	/// managers that don't pass _NET_WM_OPACITY to frame windows.
	bool detect_client_opacity;

	// === Other window processing ===
	/// Blur method for background of semi-transparent windows
	enum blur_method blur_method;
	// Size of the blur kernel
	int blur_radius;
	// Standard deviation for the gaussian blur
	double blur_deviation;
	/// Whether to blur background when the window frame is not opaque.
	/// Implies blur_background.
	bool blur_background_frame;
	/// Whether to use fixed blur strength instead of adjusting according
	/// to window opacity.
	bool blur_background_fixed;
	/// Background blur blacklist. A linked list of conditions.
	c2_lptr_t *blur_background_blacklist;
	/// Blur convolution kernel.
	struct conv **blur_kerns;
	/// Number of convolution kernels
	int blur_kernel_count;
	/// How much to dim an inactive window. 0.0 - 1.0, 0 to disable.
	double inactive_dim;
	/// Whether to use fixed inactive dim opacity, instead of deciding
	/// based on window opacity.
	bool inactive_dim_fixed;
	/// Conditions of windows to have inverted colors.
	c2_lptr_t *invert_color_list;
	/// Rules to change window opacity.
	c2_lptr_t *opacity_rules;

	// === Focus related ===
	/// Whether to try to detect WM windows and mark them as focused.
	bool mark_wmwin_focused;
	/// Whether to mark override-redirect windows as focused.
	bool mark_ovredir_focused;
	/// Whether to use EWMH _NET_ACTIVE_WINDOW to find active window.
	bool use_ewmh_active_win;
	/// A list of windows always to be considered focused.
	c2_lptr_t *focus_blacklist;
	/// Whether to do window grouping with <code>WM_TRANSIENT_FOR</code>.
	bool detect_transient;
	/// Whether to do window grouping with <code>WM_CLIENT_LEADER</code>.
	bool detect_client_leader;

	// === Calculated ===
	/// Whether compton needs to track window name and class.
	bool track_wdata;
	/// Whether compton needs to track window leaders.
	bool track_leader;

	// Don't use EWMH to detect fullscreen applications
	bool no_ewmh_fullscreen;
} options_t;

extern const char *const BACKEND_STRS[NUM_BKEND + 1];

bool must_use parse_long(const char *, long *);
bool must_use parse_int(const char *, int *);
struct conv **must_use parse_blur_kern_lst(const char *, bool *hasneg, int *count);
bool must_use parse_geometry(session_t *, const char *, region_t *);
bool must_use parse_rule_opacity(c2_lptr_t **, const char *);
enum blur_method must_use parse_blur_method(const char *src);

/**
 * Add a pattern to a condition linked list.
 */
bool condlst_add(c2_lptr_t **, const char *);

#ifdef CONFIG_LIBCONFIG
/// Parse a configuration file
/// Returns the actually config_file name used, allocated on heap
/// Outputs:
///   shadow_enable = whether shaodw is enabled globally
///   fading_enable = whether fading is enabled globally
///   win_option_mask = whether option overrides for specific window type is set for given
///                     options
///   hasneg = whether the convolution kernel has negative values
char *
parse_config_libconfig(options_t *, const char *config_file, bool *shadow_enable,
                       bool *fading_enable, bool *hasneg, win_option_mask_t *winopt_mask);
#endif

void set_default_winopts(options_t *, win_option_mask_t *, bool shadow_enable,
                         bool fading_enable);
/// Parse a configuration file is that is enabled, also initialize the winopt_mask with
/// default values
/// Outputs and returns:
///   same as parse_config_libconfig
char *parse_config(options_t *, const char *config_file, bool *shadow_enable,
                   bool *fading_enable, bool *hasneg, win_option_mask_t *winopt_mask);

/**
 * Parse a backend option argument.
 */
static inline attr_pure enum backend parse_backend(const char *str) {
	for (enum backend i = 0; BACKEND_STRS[i]; ++i) {
		if (!strcasecmp(str, BACKEND_STRS[i])) {
			return i;
		}
	}
	// Keep compatibility with an old revision containing a spelling mistake...
	if (!strcasecmp(str, "xr_glx_hybird")) {
		log_warn("backend xr_glx_hybird should be xr_glx_hybrid, the misspelt "
		         "version will be removed soon.");
		return BKEND_XR_GLX_HYBRID;
	}
	// cju wants to use dashes
	if (!strcasecmp(str, "xr-glx-hybrid")) {
		log_warn("backend xr-glx-hybrid should be xr_glx_hybrid, the alternative "
		         "version will be removed soon.");
		return BKEND_XR_GLX_HYBRID;
	}
	log_error("Invalid backend argument: %s", str);
	return NUM_BKEND;
}

/**
 * Parse a VSync option argument.
 */
static inline bool parse_vsync(const char *str) {
	if (strcmp(str, "no") == 0 || strcmp(str, "none") == 0 ||
	    strcmp(str, "false") == 0 || strcmp(str, "nah") == 0) {
		return false;
	}
	return true;
}

// vim: set noet sw=8 ts=8 :
