// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <getopt.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "common.h"
#include "config.h"
#include "log.h"
#include "options.h"
#include "utils.h"
#include "win.h"

#pragma GCC diagnostic error "-Wunused-parameter"

/**
 * Print usage text.
 */
static void usage(int ret) {
#define WARNING_DISABLED " (DISABLED AT COMPILE TIME)"
	static const char *usage_text =
	    "compton (" COMPTON_VERSION ")\n"
	    "This is the maintenance fork of compton, please report\n"
	    "bugs to https://github.com/yshui/compton\n\n"
	    "usage: compton [options]\n"
	    "Options:\n"
	    "\n"
	    "-r radius\n"
	    "  The blur radius for shadows. (default 12)\n"
	    "\n"
	    "-o opacity\n"
	    "  The translucency for shadows. (default .75)\n"
	    "\n"
	    "-l left-offset\n"
	    "  The left offset for shadows. (default -15)\n"
	    "\n"
	    "-t top-offset\n"
	    "  The top offset for shadows. (default -15)\n"
	    "\n"
	    "-I fade-in-step\n"
	    "  Opacity change between steps while fading in. (default 0.028)\n"
	    "\n"
	    "-O fade-out-step\n"
	    "  Opacity change between steps while fading out. (default 0.03)\n"
	    "\n"
	    "-D fade-delta-time\n"
	    "  The time between steps in a fade in milliseconds. (default 10)\n"
	    "\n"
	    "-m opacity\n"
	    "  The opacity for menus. (default 1.0)\n"
	    "\n"
	    "-c\n"
	    "  Enabled client-side shadows on windows.\n"
	    "\n"
	    "-C\n"
	    "  Avoid drawing shadows on dock/panel windows.\n"
	    "\n"
	    "-z\n"
	    "  Zero the part of the shadow's mask behind the window.\n"
	    "\n"
	    "-f\n"
	    "  Fade windows in/out when opening/closing and when opacity\n"
	    "  changes, unless --no-fading-openclose is used.\n"
	    "\n"
	    "-F\n"
	    "  Equals to -f. Deprecated.\n"
	    "\n"
	    "-i opacity\n"
	    "  Opacity of inactive windows. (0.1 - 1.0)\n"
	    "\n"
	    "-e opacity\n"
	    "  Opacity of window titlebars and borders. (0.1 - 1.0)\n"
	    "\n"
	    "-G\n"
	    "  Don't draw shadows on DND windows\n"
	    "\n"
	    "-b\n"
	    "  Daemonize process.\n"
	    "\n"
	    "--show-all-xerrors\n"
	    "  Show all X errors (for debugging).\n"
	    "\n"
	    "--config path\n"
	    "  Look for configuration file at the path. Use /dev/null to avoid\n"
	    "  loading configuration file."
#ifndef CONFIG_LIBCONFIG
	    WARNING_DISABLED
#endif
	    "\n\n"
	    "--write-pid-path path\n"
	    "  Write process ID to a file.\n"
	    "\n"
	    "--shadow-red value\n"
	    "  Red color value of shadow (0.0 - 1.0, defaults to 0).\n"
	    "\n"
	    "--shadow-green value\n"
	    "  Green color value of shadow (0.0 - 1.0, defaults to 0).\n"
	    "\n"
	    "--shadow-blue value\n"
	    "  Blue color value of shadow (0.0 - 1.0, defaults to 0).\n"
	    "\n"
	    "--inactive-opacity-override\n"
	    "  Inactive opacity set by -i overrides value of _NET_WM_OPACITY.\n"
	    "\n"
	    "--inactive-dim value\n"
	    "  Dim inactive windows. (0.0 - 1.0, defaults to 0)\n"
	    "\n"
	    "--active-opacity opacity\n"
	    "  Default opacity for active windows. (0.0 - 1.0)\n"
	    "\n"
	    "--mark-wmwin-focused\n"
	    "  Try to detect WM windows and mark them as active.\n"
	    "\n"
	    "--shadow-exclude condition\n"
	    "  Exclude conditions for shadows.\n"
	    "\n"
	    "--fade-exclude condition\n"
	    "  Exclude conditions for fading.\n"
	    "\n"
	    "--mark-ovredir-focused\n"
	    "  Mark windows that have no WM frame as active.\n"
	    "\n"
	    "--no-fading-openclose\n"
	    "  Do not fade on window open/close.\n"
	    "\n"
	    "--no-fading-destroyed-argb\n"
	    "  Do not fade destroyed ARGB windows with WM frame. Workaround of bugs\n"
	    "  in Openbox, Fluxbox, etc.\n"
	    "\n"
	    "--shadow-ignore-shaped\n"
	    "  Do not paint shadows on shaped windows. (Deprecated, use\n"
	    "  --shadow-exclude \'bounding_shaped\' or\n"
	    "  --shadow-exclude \'bounding_shaped && !rounded_corners\' instead.)\n"
	    "\n"
	    "--detect-rounded-corners\n"
	    "  Try to detect windows with rounded corners and don't consider\n"
	    "  them shaped windows. Affects --shadow-ignore-shaped,\n"
	    "  --unredir-if-possible, and possibly others. You need to turn this\n"
	    "  on manually if you want to match against rounded_corners in\n"
	    "  conditions.\n"
	    "\n"
	    "--detect-client-opacity\n"
	    "  Detect _NET_WM_OPACITY on client windows, useful for window\n"
	    "  managers not passing _NET_WM_OPACITY of client windows to frame\n"
	    "  windows.\n"
	    "\n"
	    "--refresh-rate val\n"
	    "  Specify refresh rate of the screen. If not specified or 0, compton\n"
	    "  will try detecting this with X RandR extension.\n"
	    "\n"
	    "--vsync\n"
	    "  Enable VSync\n"
	    "\n"
	    "--paint-on-overlay\n"
	    "  Painting on X Composite overlay window.\n"
	    "\n"
	    "--sw-opti\n"
	    "  Limit compton to repaint at most once every 1 / refresh_rate\n"
	    "  second to boost performance.\n"
	    "\n"
	    "--use-ewmh-active-win\n"
	    "  Use _NET_WM_ACTIVE_WINDOW on the root window to determine which\n"
	    "  window is focused instead of using FocusIn/Out events.\n"
	    "\n"
	    "--respect-prop-shadow\n"
	    "  Respect _COMPTON_SHADOW. This a prototype-level feature, which\n"
	    "  you must not rely on.\n"
	    "\n"
	    "--unredir-if-possible\n"
	    "  Unredirect all windows if a full-screen opaque window is\n"
	    "  detected, to maximize performance for full-screen windows.\n"
	    "\n"
	    "--unredir-if-possible-delay ms\n"
	    "  Delay before unredirecting the window, in milliseconds.\n"
	    "  Defaults to 0.\n"
	    "\n"
	    "--unredir-if-possible-exclude condition\n"
	    "  Conditions of windows that shouldn't be considered full-screen\n"
	    "  for unredirecting screen.\n"
	    "\n"
	    "--focus-exclude condition\n"
	    "  Specify a list of conditions of windows that should always be\n"
	    "  considered focused.\n"
	    "\n"
	    "--inactive-dim-fixed\n"
	    "  Use fixed inactive dim value.\n"
	    "\n"
	    "--detect-transient\n"
	    "  Use WM_TRANSIENT_FOR to group windows, and consider windows in\n"
	    "  the same group focused at the same time.\n"
	    "\n"
	    "--detect-client-leader\n"
	    "  Use WM_CLIENT_LEADER to group windows, and consider windows in\n"
	    "  the same group focused at the same time. WM_TRANSIENT_FOR has\n"
	    "  higher priority if --detect-transient is enabled, too.\n"
	    "\n"
	    "--blur-background\n"
	    "  Blur background of semi-transparent / ARGB windows. Bad in\n"
	    "  performance. The switch name may change without prior\n"
	    "  notifications.\n"
	    "\n"
	    "--blur-background-frame\n"
	    "  Blur background of windows when the window frame is not opaque.\n"
	    "  Implies --blur-background. Bad in performance. The switch name\n"
	    "  may change.\n"
	    "\n"
	    "--blur-background-fixed\n"
	    "  Use fixed blur strength instead of adjusting according to window\n"
	    "  opacity.\n"
	    "\n"
	    "--blur-kern matrix\n"
	    "  Specify the blur convolution kernel, with the following format:\n"
	    "    WIDTH,HEIGHT,ELE1,ELE2,ELE3,ELE4,ELE5...\n"
	    "  The element in the center must not be included, it will be forever\n"
	    "  1.0 or changing based on opacity, depending on whether you have\n"
	    "  --blur-background-fixed.\n"
	    "  A 7x7 Gaussian blur kernel looks like:\n"
	    "    --blur-kern "
	    "'7,7,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0."
	    "000102,0.003494,0.029143,0.059106,0.029143,0.003494,0.000102,0.000849,0."
	    "029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.001723,0.059106,0."
	    "493069,0.493069,0.059106,0.001723,0.000849,0.029143,0.243117,0.493069,0."
	    "243117,0.029143,0.000849,0.000102,0.003494,0.029143,0.059106,0.029143,0."
	    "003494,0.000102,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0."
	    "000003'\n"
	    "  Up to 4 blur kernels may be specified, separated with semicolon, for\n"
	    "  multi-pass blur.\n"
	    "  May also be one the predefined kernels: 3x3box (default), 5x5box,\n"
	    "  7x7box, 3x3gaussian, 5x5gaussian, 7x7gaussian, 9x9gaussian,\n"
	    "  11x11gaussian.\n"
	    "\n"
	    "--blur-background-exclude condition\n"
	    "  Exclude conditions for background blur.\n"
	    "\n"
	    "--resize-damage integer\n"
	    "  Resize damaged region by a specific number of pixels. A positive\n"
	    "  value enlarges it while a negative one shrinks it. Useful for\n"
	    "  fixing the line corruption issues of blur. May or may not\n"
	    "  work with --glx-no-stencil. Shrinking doesn't function correctly.\n"
	    "\n"
	    "--invert-color-include condition\n"
	    "  Specify a list of conditions of windows that should be painted with\n"
	    "  inverted color. Resource-hogging, and is not well tested.\n"
	    "\n"
	    "--opacity-rule opacity:condition\n"
	    "  Specify a list of opacity rules, in the format \"PERCENT:PATTERN\",\n"
	    "  like \'50:name *= \"Firefox\"'. compton-trans is recommended over\n"
	    "  this. Note we do not distinguish 100% and unset, and we don't make\n"
	    "  any guarantee about possible conflicts with other programs that set\n"
	    "  _NET_WM_WINDOW_OPACITY on frame or client windows.\n"
	    "\n"
	    "--shadow-exclude-reg geometry\n"
	    "  Specify a X geometry that describes the region in which shadow\n"
	    "  should not be painted in, such as a dock window region.\n"
	    "  Use --shadow-exclude-reg \'x10+0-0\', for example, if the 10 pixels\n"
	    "  on the bottom of the screen should not have shadows painted on.\n"
	    "\n"
	    "--xinerama-shadow-crop\n"
	    "  Crop shadow of a window fully on a particular Xinerama screen to the\n"
	    "  screen.\n"
	    "\n"
	    "--backend backend\n"
	    "  Choose backend. Possible choices are xrender, glx, and\n"
	    "  xr_glx_hybrid."
#ifndef CONFIG_OPENGL
	    " (GLX BACKENDS DISABLED AT COMPILE TIME)"
#endif
	    "\n\n"
	    "--glx-no-stencil\n"
	    "  GLX backend: Avoid using stencil buffer. Might cause issues\n"
	    "  when rendering transparent content. My tests show a 15% performance\n"
	    "  boost.\n"
	    "\n"
	    "--glx-no-rebind-pixmap\n"
	    "  GLX backend: Avoid rebinding pixmap on window damage. Probably\n"
	    "  could improve performance on rapid window content changes, but is\n"
	    "  known to break things on some drivers (LLVMpipe, xf86-video-intel,\n"
	    "  etc.).\n"
	    "\n"
	    "--no-use-damage\n"
	    "  Disable the use of damage information. This cause the whole screen to\n"
	    "  be redrawn everytime, instead of the part of the screen that has\n"
	    "  actually changed. Potentially degrades the performance, but might fix\n"
	    "  some artifacts.\n"
	    "\n"
	    "--xrender-sync-fence\n"
	    "  Additionally use X Sync fence to sync clients' draw calls. Needed\n"
	    "  on nvidia-drivers with GLX backend for some users.\n"
	    "\n"
	    "--force-win-blend\n"
	    "  Force all windows to be painted with blending. Useful if you have a\n"
	    "  --glx-fshader-win that could turn opaque pixels transparent.\n"
	    "\n"
	    "--dbus\n"
	    "  Enable remote control via D-Bus. See the D-BUS API section in the\n"
	    "  man page for more details."
#ifndef CONFIG_DBUS
	    WARNING_DISABLED
#endif
	    "\n\n"
	    "--benchmark cycles\n"
	    "  Benchmark mode. Repeatedly paint until reaching the specified cycles.\n"
	    "\n"
	    "--benchmark-wid window-id\n"
	    "  Specify window ID to repaint in benchmark mode. If omitted or is 0,\n"
	    "  the whole screen is repainted.\n"
	    "\n"
	    "--monitor-repaint\n"
	    "  Highlight the updated area of the screen. For debugging the xrender\n"
	    "  backend only.\n"
	    "\n"
	    "--debug-mode\n"
	    "  Render into a separate window, and don't take over the screen. Useful\n"
	    "  when you want to attach a debugger to compton\n"
	    "\n"
	    "--no-ewmh-fullscreen\n"
	    "  Do not use EWMH to detect fullscreen windows. Reverts to checking\n"
	    "  if a window is fullscreen based only on its size and coordinates.\n";
	FILE *f = (ret ? stderr : stdout);
	fputs(usage_text, f);
#undef WARNING_DISABLED
}

static const char *shortopts = "D:I:O:d:r:o:m:l:t:i:e:hscnfFCaSzGb";
static const struct option longopts[] = {
    {"help", no_argument, NULL, 'h'},
    {"config", required_argument, NULL, 256},
    {"shadow-radius", required_argument, NULL, 'r'},
    {"shadow-opacity", required_argument, NULL, 'o'},
    {"shadow-offset-x", required_argument, NULL, 'l'},
    {"shadow-offset-y", required_argument, NULL, 't'},
    {"fade-in-step", required_argument, NULL, 'I'},
    {"fade-out-step", required_argument, NULL, 'O'},
    {"fade-delta", required_argument, NULL, 'D'},
    {"menu-opacity", required_argument, NULL, 'm'},
    {"shadow", no_argument, NULL, 'c'},
    {"no-dock-shadow", no_argument, NULL, 'C'},
    {"clear-shadow", no_argument, NULL, 'z'},
    {"fading", no_argument, NULL, 'f'},
    {"inactive-opacity", required_argument, NULL, 'i'},
    {"frame-opacity", required_argument, NULL, 'e'},
    {"daemon", no_argument, NULL, 'b'},
    {"no-dnd-shadow", no_argument, NULL, 'G'},
    {"shadow-red", required_argument, NULL, 257},
    {"shadow-green", required_argument, NULL, 258},
    {"shadow-blue", required_argument, NULL, 259},
    {"inactive-opacity-override", no_argument, NULL, 260},
    {"inactive-dim", required_argument, NULL, 261},
    {"mark-wmwin-focused", no_argument, NULL, 262},
    {"shadow-exclude", required_argument, NULL, 263},
    {"mark-ovredir-focused", no_argument, NULL, 264},
    {"no-fading-openclose", no_argument, NULL, 265},
    {"shadow-ignore-shaped", no_argument, NULL, 266},
    {"detect-rounded-corners", no_argument, NULL, 267},
    {"detect-client-opacity", no_argument, NULL, 268},
    {"refresh-rate", required_argument, NULL, 269},
    {"vsync", optional_argument, NULL, 270},
    {"alpha-step", required_argument, NULL, 271},
    {"dbe", no_argument, NULL, 272},
    {"paint-on-overlay", no_argument, NULL, 273},
    {"sw-opti", no_argument, NULL, 274},
    {"vsync-aggressive", no_argument, NULL, 275},
    {"use-ewmh-active-win", no_argument, NULL, 276},
    {"respect-prop-shadow", no_argument, NULL, 277},
    {"unredir-if-possible", no_argument, NULL, 278},
    {"focus-exclude", required_argument, NULL, 279},
    {"inactive-dim-fixed", no_argument, NULL, 280},
    {"detect-transient", no_argument, NULL, 281},
    {"detect-client-leader", no_argument, NULL, 282},
    {"blur-background", no_argument, NULL, 283},
    {"blur-background-frame", no_argument, NULL, 284},
    {"blur-background-fixed", no_argument, NULL, 285},
    {"dbus", no_argument, NULL, 286},
    {"logpath", required_argument, NULL, 287},
    {"invert-color-include", required_argument, NULL, 288},
    {"opengl", no_argument, NULL, 289},
    {"backend", required_argument, NULL, 290},
    {"glx-no-stencil", no_argument, NULL, 291},
    {"glx-copy-from-front", no_argument, NULL, 292},
    {"benchmark", required_argument, NULL, 293},
    {"benchmark-wid", required_argument, NULL, 294},
    {"glx-use-copysubbuffermesa", no_argument, NULL, 295},
    {"blur-background-exclude", required_argument, NULL, 296},
    {"active-opacity", required_argument, NULL, 297},
    {"glx-no-rebind-pixmap", no_argument, NULL, 298},
    {"glx-swap-method", required_argument, NULL, 299},
    {"fade-exclude", required_argument, NULL, 300},
    {"blur-kern", required_argument, NULL, 301},
    {"resize-damage", required_argument, NULL, 302},
    {"glx-use-gpushader4", no_argument, NULL, 303},
    {"opacity-rule", required_argument, NULL, 304},
    {"shadow-exclude-reg", required_argument, NULL, 305},
    {"paint-exclude", required_argument, NULL, 306},
    {"xinerama-shadow-crop", no_argument, NULL, 307},
    {"unredir-if-possible-exclude", required_argument, NULL, 308},
    {"unredir-if-possible-delay", required_argument, NULL, 309},
    {"write-pid-path", required_argument, NULL, 310},
    {"vsync-use-glfinish", no_argument, NULL, 311},
    {"xrender-sync", no_argument, NULL, 312},
    {"xrender-sync-fence", no_argument, NULL, 313},
    {"show-all-xerrors", no_argument, NULL, 314},
    {"no-fading-destroyed-argb", no_argument, NULL, 315},
    {"force-win-blend", no_argument, NULL, 316},
    {"glx-fshader-win", required_argument, NULL, 317},
    {"version", no_argument, NULL, 318},
    {"no-x-selection", no_argument, NULL, 319},
    {"no-name-pixmap", no_argument, NULL, 320},
    {"log-level", required_argument, NULL, 321},
    {"log-file", required_argument, NULL, 322},
    {"use-damage", no_argument, NULL, 323},
    {"no-use-damage", no_argument, NULL, 324},
    {"monitor-repaint", no_argument, NULL, 800},
    {"diagnostics", no_argument, NULL, 801},
    {"debug-mode", no_argument, NULL, 802},
    {"no-ewmh-fullscreen", no_argument, NULL, 803},
    // Must terminate with a NULL entry
    {NULL, 0, NULL, 0},
};

/// Get config options that are needed to parse the rest of the options
/// Return true if we should quit
bool get_early_config(int argc, char *const *argv, char **config_file, bool *all_xerrors,
                      bool *fork, int *exit_code) {
	int o = 0, longopt_idx = -1;

	// Pre-parse the commandline arguments to check for --config and invalid
	// switches
	// Must reset optind to 0 here in case we reread the commandline
	// arguments
	optind = 1;
	*config_file = NULL;
	*exit_code = 0;
	while (-1 != (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
		if (o == 256) {
			*config_file = strdup(optarg);
		} else if (o == 'h') {
			usage(0);
			return true;

		} else if (o == 'b') {
			*fork = true;
		} else if (o == 'd') {
			log_warn("-d will be ignored, please use the DISPLAY "
			         "environment variable");
		} else if (o == 314) {
			*all_xerrors = true;
		} else if (o == 318) {
			printf("%s\n", COMPTON_VERSION);
			return true;
		} else if (o == 'S') {
			log_warn("-S will be ignored");
		} else if (o == 320) {
			log_warn("--no-name-pixmap will be ignored");
		} else if (o == '?' || o == ':') {
			usage(1);
			*exit_code = 1;
			return true;
		}
	}

	// Check for abundant positional arguments
	if (optind < argc) {
		// log is not initialized here yet
		fprintf(stderr, "compton doesn't accept positional arguments.\n");
		*exit_code = 1;
		return true;
	}

	return false;
}

/**
 * Process arguments and configuration files.
 */
void get_cfg(options_t *opt, int argc, char *const *argv, bool shadow_enable,
             bool fading_enable, bool conv_kern_hasneg, win_option_mask_t *winopt_mask) {

	int o = 0, longopt_idx = -1;

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));

	// Enforce LC_NUMERIC locale "C" here to make sure dots are recognized
	// instead of commas in atof().
	setlocale(LC_NUMERIC, "C");

	// Parse commandline arguments. Range checking will be done later.

	const char *deprecation_message = "has been removed. If you encounter problems "
	                                  "without this feature, please feel free to "
	                                  "open a bug report.";
	optind = 1;
	while (-1 != (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
		switch (o) {
#define P_CASEBOOL(idx, option)                                                          \
	case idx:                                                                        \
		opt->option = true;                                                      \
		break
#define P_CASELONG(idx, option)                                                          \
	case idx:                                                                        \
		if (!parse_long(optarg, &opt->option)) {                                 \
			exit(1);                                                         \
		}                                                                        \
		break
#define P_CASEINT(idx, option)                                                           \
	case idx:                                                                        \
		if (!parse_int(optarg, &opt->option)) {                                  \
			exit(1);                                                         \
		}                                                                        \
		break

		// clang-format off
		// Short options
		case 318:
		case 'h':
			// These options should cause compton to exit early,
			// so assert(false) here
			assert(false);
			break;
		case 'd':
		case 'b':
		case 'S':
		case 314:
		case 320:
			// These options are handled by get_early_config()
			break;
		P_CASEINT('D', fade_delta);
		case 'I': opt->fade_in_step = normalize_d(atof(optarg)); break;
		case 'O': opt->fade_out_step = normalize_d(atof(optarg)); break;
		case 'c': shadow_enable = true; break;
		case 'C':
			winopt_mask[WINTYPE_DOCK].shadow = true;
			opt->wintype_option[WINTYPE_DOCK].shadow = false;
			break;
		case 'G':
			winopt_mask[WINTYPE_DND].shadow = true;
			opt->wintype_option[WINTYPE_DND].shadow = false;
			break;
		case 'm':;
			double tmp;
			tmp = normalize_d(atof(optarg));
			winopt_mask[WINTYPE_DROPDOWN_MENU].opacity = true;
			winopt_mask[WINTYPE_POPUP_MENU].opacity = true;
			opt->wintype_option[WINTYPE_POPUP_MENU].opacity = tmp;
			opt->wintype_option[WINTYPE_DROPDOWN_MENU].opacity = tmp;
			break;
		case 'f':
		case 'F':
			fading_enable = true;
			break;
		P_CASEINT('r', shadow_radius);
		case 'o':
			opt->shadow_opacity = atof(optarg);
			break;
		P_CASEINT('l', shadow_offset_x);
		P_CASEINT('t', shadow_offset_y);
		case 'i':
			opt->inactive_opacity = normalize_d(atof(optarg));
			break;
		case 'e': opt->frame_opacity = atof(optarg); break;
		case 'z':
			log_warn("clear-shadow is removed, shadows are automatically "
			         "cleared now. If you want to prevent shadow from been "
			         "cleared under certain types of windows, you can use "
			         "the \"full-shadow\" per window type option.");
			break;
		case 'n':
		case 'a':
		case 's':
			log_error("-n, -a, and -s have been removed.");
			break;
		// Long options
		case 256:
			// --config
			break;
		case 257:
			// --shadow-red
			opt->shadow_red = atof(optarg);
			break;
		case 258:
			// --shadow-green
			opt->shadow_green = atof(optarg);
			break;
		case 259:
			// --shadow-blue
			opt->shadow_blue = atof(optarg);
			break;
		P_CASEBOOL(260, inactive_opacity_override);
		case 261:
			// --inactive-dim
			opt->inactive_dim = atof(optarg);
			break;
		P_CASEBOOL(262, mark_wmwin_focused);
		case 263:
			// --shadow-exclude
			condlst_add(&opt->shadow_blacklist, optarg);
			break;
		P_CASEBOOL(264, mark_ovredir_focused);
		P_CASEBOOL(265, no_fading_openclose);
		P_CASEBOOL(266, shadow_ignore_shaped);
		P_CASEBOOL(267, detect_rounded_corners);
		P_CASEBOOL(268, detect_client_opacity);
		P_CASEINT(269, refresh_rate);
		case 270:
			if (optarg) {
				opt->vsync = parse_vsync(optarg);
				log_warn("--vsync doesn't take argument anymore. \"%s\" "
					 "is interpreted as \"%s\" for compatibility, but "
					 "this will stop working soon",
					 optarg, opt->vsync ? "true" : "false");
			} else {
				opt->vsync = true;
			}
			break;
		case 271:
			// --alpha-step
			log_warn("--alpha-step has been removed, compton now tries to "
			         "make use of all alpha values");
			break;
		case 272: log_warn("use of --dbe is deprecated"); break;
		case 273:
			log_warn("--paint-on-overlay has been removed, and is enabled "
			         "when possible");
			break;
		P_CASEBOOL(274, sw_opti);
		case 275:
			// --vsync-aggressive
			log_warn("--vsync-aggressive has been deprecated, please remove it"
			         " from the command line options");
			break;
		P_CASEBOOL(276, use_ewmh_active_win);
		P_CASEBOOL(277, respect_prop_shadow);
		P_CASEBOOL(278, unredir_if_possible);
		case 279:
			// --focus-exclude
			condlst_add(&opt->focus_blacklist, optarg);
			break;
		P_CASEBOOL(280, inactive_dim_fixed);
		P_CASEBOOL(281, detect_transient);
		P_CASEBOOL(282, detect_client_leader);
		case 283:
			// --blur_background
			opt->blur_method = BLUR_METHOD_KERNEL;
			break;
		P_CASEBOOL(284, blur_background_frame);
		P_CASEBOOL(285, blur_background_fixed);
		P_CASEBOOL(286, dbus);
		case 287:
			log_warn("Please use --log-file instead of --logpath");
			// fallthrough
		case 322:
			// --logpath, --log-file
			free(opt->logpath);
			opt->logpath = strdup(optarg);
			break;
		case 288:
			// --invert-color-include
			condlst_add(&opt->invert_color_list, optarg);
			break;
		case 289:
			// --opengl
			opt->backend = BKEND_GLX;
			break;
		case 290:
			// --backend
			opt->backend = parse_backend(optarg);
			if (opt->backend >= NUM_BKEND)
				exit(1);
			break;
		P_CASEBOOL(291, glx_no_stencil);
		case 292:
			log_error("--glx-copy-from-front %s", deprecation_message);
			exit(1);
			break;
		P_CASEINT(293, benchmark);
		case 294:
			// --benchmark-wid
			opt->benchmark_wid = (xcb_window_t)strtol(optarg, NULL, 0);
			break;
		case 295:
			log_error("--glx-use-copysubbuffermesa %s", deprecation_message);
			exit(1);
			break;
		case 296:
			// --blur-background-exclude
			condlst_add(&opt->blur_background_blacklist, optarg);
			break;
		case 297:
			// --active-opacity
			opt->active_opacity = normalize_d(atof(optarg));
			break;
		P_CASEBOOL(298, glx_no_rebind_pixmap);
		case 299: {
			// --glx-swap-method
			char *endptr;
			long tmpval = strtol(optarg, &endptr, 10);
			bool should_remove = true;
			if (*endptr || !(*optarg)) {
				// optarg is not a number, or an empty string
				tmpval = -1;
			}
			if (strcmp(optarg, "undefined") != 0 && tmpval != 0) {
				// If not undefined, we will use damage and buffer-age to
				// limit the rendering area.
				opt->use_damage = true;
				should_remove = false;
			}
			log_warn("--glx-swap-method has been deprecated, your setting "
			         "\"%s\" should be %s.",
			         optarg,
			         !should_remove ? "replaced by `--use-damage`" :
			                         "removed");
			break;
		}
		case 300:
			// --fade-exclude
			condlst_add(&opt->fade_blacklist, optarg);
			break;
		case 301:
			// --blur-kern
			opt->blur_kerns = parse_blur_kern_lst(optarg, &conv_kern_hasneg,
			                                      &opt->blur_kernel_count);
			if (!opt->blur_kerns) {
				exit(1);
			}
			break;
		P_CASEINT(302, resize_damage);
		case 303:
			// --glx-use-gpushader4
			log_warn("--glx-use-gpushader4 is deprecated since v6."
			         " Please remove it from command line options.");
			break;
		case 304:
			// --opacity-rule
			if (!parse_rule_opacity(&opt->opacity_rules, optarg))
				exit(1);
			break;
		case 305:
			// --shadow-exclude-reg
			free(opt->shadow_exclude_reg_str);
			opt->shadow_exclude_reg_str = strdup(optarg);
			log_warn("--shadow-exclude-reg is deprecated. You are likely "
			         "better off using --shadow-exclude anyway");
			break;
		case 306:
			// --paint-exclude
			condlst_add(&opt->paint_blacklist, optarg);
			break;
		P_CASEBOOL(307, xinerama_shadow_crop);
		case 308:
			// --unredir-if-possible-exclude
			condlst_add(&opt->unredir_if_possible_blacklist, optarg);
			break;
		P_CASELONG(309, unredir_if_possible_delay);
		case 310:
			// --write-pid-path
			opt->write_pid_path = strdup(optarg);
			break;
		P_CASEBOOL(311, vsync_use_glfinish);
		case 312:
			// --xrender-sync
			log_warn("Please use --xrender-sync-fence instead of --xrender-sync");
			opt->xrender_sync_fence = true;
			break;
		P_CASEBOOL(313, xrender_sync_fence);
		P_CASEBOOL(315, no_fading_destroyed_argb);
		P_CASEBOOL(316, force_win_blend);
		case 317:
			opt->glx_fshader_win_str = strdup(optarg);
			break;
		case 321: {
			enum log_level tmp_level = string_to_log_level(optarg);
			if (tmp_level == LOG_LEVEL_INVALID) {
				log_warn("Invalid log level, defaults to WARN");
			} else {
				log_set_level_tls(tmp_level);
			}
			break;
		}
		P_CASEBOOL(319, no_x_selection);
		P_CASEBOOL(323, use_damage);
		case 324:
			opt->use_damage = false;
			break;
		P_CASEBOOL(800, monitor_repaint);
		case 801: opt->print_diagnostics = true; break;
		P_CASEBOOL(802, debug_mode);
		P_CASEBOOL(803, no_ewmh_fullscreen);
		default: usage(1); break;
#undef P_CASEBOOL
		}
		// clang-format on
	}

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	// Range checking and option assignments
	opt->fade_delta = max2(opt->fade_delta, 1);
	opt->shadow_radius = max2(opt->shadow_radius, 0);
	opt->shadow_red = normalize_d(opt->shadow_red);
	opt->shadow_green = normalize_d(opt->shadow_green);
	opt->shadow_blue = normalize_d(opt->shadow_blue);
	opt->inactive_dim = normalize_d(opt->inactive_dim);
	opt->frame_opacity = normalize_d(opt->frame_opacity);
	opt->shadow_opacity = normalize_d(opt->shadow_opacity);
	opt->refresh_rate = normalize_i_range(opt->refresh_rate, 0, 300);

	// Apply default wintype options that are dependent on global options
	set_default_winopts(opt, winopt_mask, shadow_enable, fading_enable);

	// --blur-background-frame implies --blur-background
	if (opt->blur_background_frame && !opt->blur_method) {
		opt->blur_method = BLUR_METHOD_KERNEL;
	}

	// Other variables determined by options

	// Determine whether we track window grouping
	if (opt->detect_transient || opt->detect_client_leader) {
		opt->track_leader = true;
	}

	// Fill default blur kernel
	if (opt->blur_method == BLUR_METHOD_KERNEL &&
	    (!opt->blur_kerns || !opt->blur_kerns[0])) {
		opt->blur_kerns = parse_blur_kern_lst("3x3box", &conv_kern_hasneg,
		                                      &opt->blur_kernel_count);
		CHECK(opt->blur_kerns);
		CHECK(opt->blur_kernel_count);
	}

	if (opt->resize_damage < 0) {
		log_warn("Negative --resize-damage will not work correctly.");
	}

	if (opt->backend == BKEND_XRENDER && conv_kern_hasneg) {
		log_warn("A convolution kernel with negative values may not work "
		         "properly under X Render backend.");
	}
}

// vim: set noet sw=8 ts=8 :
