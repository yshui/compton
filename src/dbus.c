// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <X11/Xlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "list.h"
#include "log.h"
#include "string_utils.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "win.h"

#include "dbus.h"

struct cdbus_data {
	/// DBus connection.
	DBusConnection *dbus_conn;
	/// DBus service name.
	char *dbus_service;
};

// Window type
typedef uint32_t cdbus_window_t;
#define CDBUS_TYPE_WINDOW DBUS_TYPE_UINT32
#define CDBUS_TYPE_WINDOW_STR DBUS_TYPE_UINT32_AS_STRING

typedef uint32_t cdbus_enum_t;
#define CDBUS_TYPE_ENUM DBUS_TYPE_UINT32
#define CDBUS_TYPE_ENUM_STR DBUS_TYPE_UINT32_AS_STRING

#define CDBUS_SERVICE_NAME "com.github.chjj.compton"
#define CDBUS_INTERFACE_NAME CDBUS_SERVICE_NAME
#define CDBUS_OBJECT_NAME "/com/github/chjj/compton"
#define CDBUS_ERROR_PREFIX CDBUS_INTERFACE_NAME ".error"
#define CDBUS_ERROR_UNKNOWN CDBUS_ERROR_PREFIX ".unknown"
#define CDBUS_ERROR_UNKNOWN_S "Well, I don't know what happened. Do you?"
#define CDBUS_ERROR_BADMSG CDBUS_ERROR_PREFIX ".bad_message"
#define CDBUS_ERROR_BADMSG_S                                                             \
	"Unrecognized command. Beware compton "                                          \
	"cannot make you a sandwich."
#define CDBUS_ERROR_BADARG CDBUS_ERROR_PREFIX ".bad_argument"
#define CDBUS_ERROR_BADARG_S "Failed to parse argument %d: %s"
#define CDBUS_ERROR_BADWIN CDBUS_ERROR_PREFIX ".bad_window"
#define CDBUS_ERROR_BADWIN_S "Requested window %#010x not found."
#define CDBUS_ERROR_BADTGT CDBUS_ERROR_PREFIX ".bad_target"
#define CDBUS_ERROR_BADTGT_S "Target \"%s\" not found."
#define CDBUS_ERROR_FORBIDDEN CDBUS_ERROR_PREFIX ".forbidden"
#define CDBUS_ERROR_FORBIDDEN_S "Incorrect password, access denied."
#define CDBUS_ERROR_CUSTOM CDBUS_ERROR_PREFIX ".custom"
#define CDBUS_ERROR_CUSTOM_S "%s"

#define cdbus_reply_err(ps, srcmsg, err_name, err_format, ...)                           \
	cdbus_reply_errm((ps), dbus_message_new_error_printf(                            \
	                           (srcmsg), (err_name), (err_format), ##__VA_ARGS__))

static DBusHandlerResult cdbus_process(DBusConnection *conn, DBusMessage *m, void *);

static dbus_bool_t cdbus_callback_add_timeout(DBusTimeout *timeout, void *data);

static void cdbus_callback_remove_timeout(DBusTimeout *timeout, void *data);

static void cdbus_callback_timeout_toggled(DBusTimeout *timeout, void *data);

static dbus_bool_t cdbus_callback_add_watch(DBusWatch *watch, void *data);

static void cdbus_callback_remove_watch(DBusWatch *watch, void *data);

static void cdbus_callback_watch_toggled(DBusWatch *watch, void *data);

/**
 * Initialize D-Bus connection.
 */
bool cdbus_init(session_t *ps, const char *uniq) {
	auto cd = cmalloc(struct cdbus_data);
	cd->dbus_service = NULL;

	// Set ps->dbus_data here because add_watch functions need it
	ps->dbus_data = cd;

	DBusError err = {};

	// Initialize
	dbus_error_init(&err);

	// Connect to D-Bus
	// Use dbus_bus_get_private() so we can fully recycle it ourselves
	cd->dbus_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (dbus_error_is_set(&err)) {
		log_error("D-Bus connection failed (%s).", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	if (!cd->dbus_conn) {
		log_error("D-Bus connection failed for unknown reason.");
		goto fail;
	}

	// Avoid exiting on disconnect
	dbus_connection_set_exit_on_disconnect(cd->dbus_conn, false);

	// Request service name
	{
		// Build service name
		size_t service_len = strlen(CDBUS_SERVICE_NAME) + strlen(uniq) + 2;
		char *service = ccalloc(service_len, char);
		snprintf(service, service_len, "%s.%s", CDBUS_SERVICE_NAME, uniq);

		// Make a valid dbus name by converting non alphanumeric characters to
		// underscore
		char *tmp = service + strlen(CDBUS_SERVICE_NAME) + 1;
		while (*tmp) {
			if (!isalnum(*tmp)) {
				*tmp = '_';
			}
			tmp++;
		}
		cd->dbus_service = service;

		// Request for the name
		int ret = dbus_bus_request_name(cd->dbus_conn, service,
		                                DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);

		if (dbus_error_is_set(&err)) {
			log_error("Failed to obtain D-Bus name (%s).", err.message);
			dbus_error_free(&err);
			goto fail;
		}

		if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret &&
		    DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER != ret) {
			log_error("Failed to become the primary owner of requested D-Bus "
			          "name (%d).",
			          ret);
			goto fail;
		}
	}

	// Add watch handlers
	if (!dbus_connection_set_watch_functions(cd->dbus_conn, cdbus_callback_add_watch,
	                                         cdbus_callback_remove_watch,
	                                         cdbus_callback_watch_toggled, ps, NULL)) {
		log_error("Failed to add D-Bus watch functions.");
		goto fail;
	}

	// Add timeout handlers
	if (!dbus_connection_set_timeout_functions(
	        cd->dbus_conn, cdbus_callback_add_timeout, cdbus_callback_remove_timeout,
	        cdbus_callback_timeout_toggled, ps, NULL)) {
		log_error("Failed to add D-Bus timeout functions.");
		goto fail;
	}

	// Add match
	dbus_bus_add_match(cd->dbus_conn,
	                   "type='method_call',interface='" CDBUS_INTERFACE_NAME "'", &err);
	if (dbus_error_is_set(&err)) {
		log_error("Failed to add D-Bus match.");
		dbus_error_free(&err);
		goto fail;
	}
	dbus_connection_add_filter(cd->dbus_conn, cdbus_process, ps, NULL);
	return true;
fail:
	ps->dbus_data = NULL;
	free(cd->dbus_service);
	free(cd);
	return false;
}

/**
 * Destroy D-Bus connection.
 */
void cdbus_destroy(session_t *ps) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn) {
		// Release DBus name firstly
		if (cd->dbus_service) {
			DBusError err = {};
			dbus_error_init(&err);

			dbus_bus_release_name(cd->dbus_conn, cd->dbus_service, &err);
			if (dbus_error_is_set(&err)) {
				log_error("Failed to release DBus name (%s).", err.message);
				dbus_error_free(&err);
			}
			free(cd->dbus_service);
		}

		// Close and unref the connection
		dbus_connection_close(cd->dbus_conn);
		dbus_connection_unref(cd->dbus_conn);
	}
	free(cd);
}

/** @name DBusTimeout handling
 */
///@{

typedef struct ev_dbus_timer {
	ev_timer w;
	DBusTimeout *t;
} ev_dbus_timer;

/**
 * Callback for handling a D-Bus timeout.
 */
static void cdbus_callback_handle_timeout(EV_P attr_unused, ev_timer *w, int revents attr_unused) {
	ev_dbus_timer *t = (void *)w;
	dbus_timeout_handle(t->t);
}

/**
 * Callback for adding D-Bus timeout.
 */
static dbus_bool_t cdbus_callback_add_timeout(DBusTimeout *timeout, void *data) {
	session_t *ps = data;

	auto t = ccalloc(1, ev_dbus_timer);
	double i = dbus_timeout_get_interval(timeout) / 1000.0;
	ev_timer_init(&t->w, cdbus_callback_handle_timeout, i, i);
	t->t = timeout;
	dbus_timeout_set_data(timeout, t, NULL);

	if (dbus_timeout_get_enabled(timeout))
		ev_timer_start(ps->loop, &t->w);

	return true;
}

/**
 * Callback for removing D-Bus timeout.
 */
static void cdbus_callback_remove_timeout(DBusTimeout *timeout, void *data) {
	session_t *ps = data;

	ev_dbus_timer *t = dbus_timeout_get_data(timeout);
	assert(t);
	ev_timer_stop(ps->loop, &t->w);
	free(t);
}

/**
 * Callback for toggling a D-Bus timeout.
 */
static void cdbus_callback_timeout_toggled(DBusTimeout *timeout, void *data) {
	session_t *ps = data;
	ev_dbus_timer *t = dbus_timeout_get_data(timeout);

	assert(t);
	ev_timer_stop(ps->loop, &t->w);
	if (dbus_timeout_get_enabled(timeout)) {
		double i = dbus_timeout_get_interval(timeout) / 1000.0;
		ev_timer_set(&t->w, i, i);
		ev_timer_start(ps->loop, &t->w);
	}
}

///@}

/** @name DBusWatch handling
 */
///@{

typedef struct ev_dbus_io {
	ev_io w;
	struct cdbus_data *cd;
	DBusWatch *dw;
} ev_dbus_io;

void cdbus_io_callback(EV_P attr_unused, ev_io *w, int revents) {
	ev_dbus_io *dw = (void *)w;
	DBusWatchFlags flags = 0;
	if (revents & EV_READ)
		flags |= DBUS_WATCH_READABLE;
	if (revents & EV_WRITE)
		flags |= DBUS_WATCH_WRITABLE;
	dbus_watch_handle(dw->dw, flags);
	while (dbus_connection_dispatch(dw->cd->dbus_conn) != DBUS_DISPATCH_COMPLETE)
		;
}

/**
 * Determine the poll condition of a DBusWatch.
 */
static inline int cdbus_get_watch_cond(DBusWatch *watch) {
	const unsigned flags = dbus_watch_get_flags(watch);
	int condition = 0;
	if (flags & DBUS_WATCH_READABLE)
		condition |= EV_READ;
	if (flags & DBUS_WATCH_WRITABLE)
		condition |= EV_WRITE;

	return condition;
}

/**
 * Callback for adding D-Bus watch.
 */
static dbus_bool_t cdbus_callback_add_watch(DBusWatch *watch, void *data) {
	session_t *ps = data;

	auto w = ccalloc(1, ev_dbus_io);
	w->dw = watch;
	w->cd = ps->dbus_data;
	ev_io_init(&w->w, cdbus_io_callback, dbus_watch_get_unix_fd(watch),
	           cdbus_get_watch_cond(watch));

	// Leave disabled watches alone
	if (dbus_watch_get_enabled(watch))
		ev_io_start(ps->loop, &w->w);

	dbus_watch_set_data(watch, w, NULL);

	// Always return true
	return true;
}

/**
 * Callback for removing D-Bus watch.
 */
static void cdbus_callback_remove_watch(DBusWatch *watch, void *data) {
	session_t *ps = data;
	ev_dbus_io *w = dbus_watch_get_data(watch);
	ev_io_stop(ps->loop, &w->w);
	free(w);
}

/**
 * Callback for toggling D-Bus watch status.
 */
static void cdbus_callback_watch_toggled(DBusWatch *watch, void *data) {
	session_t *ps = data;
	ev_io *w = dbus_watch_get_data(watch);
	if (dbus_watch_get_enabled(watch))
		ev_io_start(ps->loop, w);
	else
		ev_io_stop(ps->loop, w);
}

///@}

/** @name Message argument appending callbacks
 */
///@{

/**
 * Callback to append a bool argument to a message.
 */
static bool cdbus_apdarg_bool(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	assert(data);

	dbus_bool_t val = *(const bool *)data;

	if (!dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append an int32 argument to a message.
 */
static bool cdbus_apdarg_int32(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	if (!dbus_message_append_args(msg, DBUS_TYPE_INT32, data, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append an uint32 argument to a message.
 */
static bool
cdbus_apdarg_uint32(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	if (!dbus_message_append_args(msg, DBUS_TYPE_UINT32, data, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append a double argument to a message.
 */
static bool
cdbus_apdarg_double(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	if (!dbus_message_append_args(msg, DBUS_TYPE_DOUBLE, data, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append a Window argument to a message.
 */
static bool cdbus_apdarg_wid(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	assert(data);
	cdbus_window_t val = *(const xcb_window_t *)data;

	if (!dbus_message_append_args(msg, CDBUS_TYPE_WINDOW, &val, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append an cdbus_enum_t argument to a message.
 */
static bool cdbus_apdarg_enum(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	assert(data);
	if (!dbus_message_append_args(msg, CDBUS_TYPE_ENUM, data, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append a string argument to a message.
 */
static bool
cdbus_apdarg_string(session_t *ps attr_unused, DBusMessage *msg, const void *data) {
	const char *str = data;
	if (!str)
		str = "";

	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &str, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		return false;
	}

	return true;
}

/**
 * Callback to append all window IDs to a message.
 */
static bool cdbus_apdarg_wids(session_t *ps, DBusMessage *msg, const void *data attr_unused) {
	// Get the number of wids we are to include
	unsigned count = 0;
	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		++count;
	}

	if (!count) {
		// Nothing to append
		return true;
	}

	// Allocate memory for an array of window IDs
	auto arr = ccalloc(count, cdbus_window_t);

	// Build the array
	cdbus_window_t *pcur = arr;
	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		*pcur = w->id;
		++pcur;
	}
	assert(pcur == arr + count);

	// Append arguments
	if (!dbus_message_append_args(msg, DBUS_TYPE_ARRAY, CDBUS_TYPE_WINDOW, &arr,
	                              count, DBUS_TYPE_INVALID)) {
		log_error("Failed to append argument.");
		free(arr);
		return false;
	}

	free(arr);
	return true;
}
///@}

/**
 * Send a D-Bus signal.
 *
 * @param ps current session
 * @param name signal name
 * @param func a function that modifies the built message, to, for example,
 *        add an argument
 * @param data data pointer to pass to the function
 */
static bool cdbus_signal(session_t *ps, const char *name,
                         bool (*func)(session_t *ps, DBusMessage *msg, const void *data),
                         const void *data) {
	struct cdbus_data *cd = ps->dbus_data;
	DBusMessage *msg = NULL;

	// Create a signal
	msg = dbus_message_new_signal(CDBUS_OBJECT_NAME, CDBUS_INTERFACE_NAME, name);
	if (!msg) {
		log_error("Failed to create D-Bus signal.");
		return false;
	}

	// Append arguments onto message
	if (func && !func(ps, msg, data)) {
		dbus_message_unref(msg);
		return false;
	}

	// Send the message and flush the connection
	if (!dbus_connection_send(cd->dbus_conn, msg, NULL)) {
		log_error("Failed to send D-Bus signal.");
		dbus_message_unref(msg);
		return false;
	}
	dbus_connection_flush(cd->dbus_conn);

	// Free the message
	dbus_message_unref(msg);

	return true;
}

/**
 * Send a signal with a Window ID as argument.
 */
static inline bool cdbus_signal_wid(session_t *ps, const char *name, xcb_window_t wid) {
	return cdbus_signal(ps, name, cdbus_apdarg_wid, &wid);
}

/**
 * Send a D-Bus reply.
 *
 * @param ps current session
 * @param srcmsg original message
 * @param func a function that modifies the built message, to, for example,
 *        add an argument
 * @param data data pointer to pass to the function
 */
static bool cdbus_reply(session_t *ps, DBusMessage *srcmsg,
                        bool (*func)(session_t *ps, DBusMessage *msg, const void *data),
                        const void *data) {
	struct cdbus_data *cd = ps->dbus_data;
	DBusMessage *msg = NULL;

	// Create a reply
	msg = dbus_message_new_method_return(srcmsg);
	if (!msg) {
		log_error("Failed to create D-Bus reply.");
		return false;
	}

	// Append arguments onto message
	if (func && !func(ps, msg, data)) {
		dbus_message_unref(msg);
		return false;
	}

	// Send the message and flush the connection
	if (!dbus_connection_send(cd->dbus_conn, msg, NULL)) {
		log_error("Failed to send D-Bus reply.");
		dbus_message_unref(msg);
		return false;
	}
	dbus_connection_flush(cd->dbus_conn);

	// Free the message
	dbus_message_unref(msg);

	return true;
}

/**
 * Send a reply with a bool argument.
 */
static inline bool cdbus_reply_bool(session_t *ps, DBusMessage *srcmsg, bool bval) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_bool, &bval);
}

/**
 * Send a reply with an int32 argument.
 */
static inline bool cdbus_reply_int32(session_t *ps, DBusMessage *srcmsg, int32_t val) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_int32, &val);
}

/**
 * Send a reply with an int32 argument, cast from a long.
 */
static inline bool cdbus_reply_int32l(session_t *ps, DBusMessage *srcmsg, long val) {
	int32_t tmp = (int32_t)val;
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_int32, &tmp);
}

/**
 * Send a reply with an uint32 argument.
 */
static inline bool cdbus_reply_uint32(session_t *ps, DBusMessage *srcmsg, uint32_t val) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_uint32, &val);
}

/**
 * Send a reply with a double argument.
 */
static inline bool cdbus_reply_double(session_t *ps, DBusMessage *srcmsg, double val) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_double, &val);
}

/**
 * Send a reply with a wid argument.
 */
static inline bool cdbus_reply_wid(session_t *ps, DBusMessage *srcmsg, xcb_window_t wid) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_wid, &wid);
}

/**
 * Send a reply with a string argument.
 */
static inline bool cdbus_reply_string(session_t *ps, DBusMessage *srcmsg, const char *str) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_string, str);
}

/**
 * Send a reply with a enum argument.
 */
static inline bool cdbus_reply_enum(session_t *ps, DBusMessage *srcmsg, cdbus_enum_t eval) {
	return cdbus_reply(ps, srcmsg, cdbus_apdarg_enum, &eval);
}

/**
 * Send a D-Bus error reply.
 *
 * @param ps current session
 * @param msg the new error DBusMessage
 */
static bool cdbus_reply_errm(session_t *ps, DBusMessage *msg) {
	struct cdbus_data *cd = ps->dbus_data;
	if (!msg) {
		log_error("Failed to create D-Bus reply.");
		return false;
	}

	// Send the message and flush the connection
	if (!dbus_connection_send(cd->dbus_conn, msg, NULL)) {
		log_error("Failed to send D-Bus reply.");
		dbus_message_unref(msg);
		return false;
	}
	dbus_connection_flush(cd->dbus_conn);

	// Free the message
	dbus_message_unref(msg);

	return true;
}

/**
 * Get n-th argument of a D-Bus message.
 *
 * @param count the position of the argument to get, starting from 0
 * @param type libdbus type number of the type
 * @param pdest pointer to the target
 * @return true if successful, false otherwise.
 */
static bool cdbus_msg_get_arg(DBusMessage *msg, int count, const int type, void *pdest) {
	assert(count >= 0);

	DBusMessageIter iter = {};
	if (!dbus_message_iter_init(msg, &iter)) {
		log_error("Message has no argument.");
		return false;
	}

	{
		const int oldcount = count;
		while (count) {
			if (!dbus_message_iter_next(&iter)) {
				log_error("Failed to find argument %d.", oldcount);
				return false;
			}
			--count;
		}
	}

	if (type != dbus_message_iter_get_arg_type(&iter)) {
		log_error("Argument has incorrect type.");
		return false;
	}

	dbus_message_iter_get_basic(&iter, pdest);

	return true;
}

/** @name Message processing
 */
///@{

/**
 * Process a list_win D-Bus request.
 */
static bool cdbus_process_list_win(session_t *ps, DBusMessage *msg) {
	cdbus_reply(ps, msg, cdbus_apdarg_wids, NULL);

	return true;
}

/**
 * Process a win_get D-Bus request.
 */
static bool cdbus_process_win_get(session_t *ps, DBusMessage *msg) {
	cdbus_window_t wid = XCB_NONE;
	const char *target = NULL;
	DBusError err = {};

	if (!dbus_message_get_args(msg, &err, CDBUS_TYPE_WINDOW, &wid, DBUS_TYPE_STRING,
	                           &target, DBUS_TYPE_INVALID)) {
		log_error("Failed to parse argument of \"win_get\" (%s).", err.message);
		dbus_error_free(&err);
		return false;
	}

	auto w = find_managed_win(ps, wid);

	if (!w) {
		log_error("Window %#010x not found.", wid);
		cdbus_reply_err(ps, msg, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return true;
	}

#define cdbus_m_win_get_do(tgt, apdarg_func)                                             \
	if (!strcmp(#tgt, target)) {                                                     \
		apdarg_func(ps, msg, w->tgt);                                            \
		return true;                                                             \
	}

	cdbus_m_win_get_do(base.id, cdbus_reply_wid);

	// next
	if (!strcmp("next", target)) {
		cdbus_reply_wid(
		    ps, msg,
		    (list_node_is_last(&ps->window_stack, &w->base.stack_neighbour)
		         ? 0
		         : list_entry(w->base.stack_neighbour.next, struct win, stack_neighbour)
		               ->id));
		return true;
	}

	// map_state
	if (!strcmp("map_state", target)) {
		cdbus_reply_bool(ps, msg, w->a.map_state);
		return true;
	}

	cdbus_m_win_get_do(mode, cdbus_reply_enum);
	cdbus_m_win_get_do(client_win, cdbus_reply_wid);
	cdbus_m_win_get_do(ever_damaged, cdbus_reply_bool);
	cdbus_m_win_get_do(window_type, cdbus_reply_enum);
	cdbus_m_win_get_do(wmwin, cdbus_reply_bool);
	cdbus_m_win_get_do(leader, cdbus_reply_wid);
	// focused_real
	if (!strcmp("focused_real", target)) {
		cdbus_reply_bool(ps, msg, win_is_focused_real(ps, w));
		return true;
	}
	cdbus_m_win_get_do(fade_force, cdbus_reply_enum);
	cdbus_m_win_get_do(shadow_force, cdbus_reply_enum);
	cdbus_m_win_get_do(focused_force, cdbus_reply_enum);
	cdbus_m_win_get_do(invert_color_force, cdbus_reply_enum);
	cdbus_m_win_get_do(name, cdbus_reply_string);
	cdbus_m_win_get_do(class_instance, cdbus_reply_string);
	cdbus_m_win_get_do(class_general, cdbus_reply_string);
	cdbus_m_win_get_do(role, cdbus_reply_string);

	cdbus_m_win_get_do(opacity, cdbus_reply_double);
	cdbus_m_win_get_do(opacity_target, cdbus_reply_double);
	cdbus_m_win_get_do(has_opacity_prop, cdbus_reply_bool);
	cdbus_m_win_get_do(opacity_prop, cdbus_reply_uint32);
	cdbus_m_win_get_do(opacity_is_set, cdbus_reply_bool);
	cdbus_m_win_get_do(opacity_set, cdbus_reply_double);

	cdbus_m_win_get_do(frame_opacity, cdbus_reply_double);
	if (!strcmp("left_width", target)) {
		cdbus_reply_int32(ps, msg, w->frame_extents.left);
		return true;
	}
	if (!strcmp("right_width", target)) {
		cdbus_reply_int32(ps, msg, w->frame_extents.right);
		return true;
	}
	if (!strcmp("top_width", target)) {
		cdbus_reply_int32(ps, msg, w->frame_extents.top);
		return true;
	}
	if (!strcmp("bottom_width", target)) {
		cdbus_reply_int32(ps, msg, w->frame_extents.bottom);
		return true;
	}

	cdbus_m_win_get_do(shadow, cdbus_reply_bool);
	cdbus_m_win_get_do(invert_color, cdbus_reply_bool);
	cdbus_m_win_get_do(blur_background, cdbus_reply_bool);
#undef cdbus_m_win_get_do

	log_error(CDBUS_ERROR_BADTGT_S, target);
	cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

	return true;
}

/**
 * Process a win_set D-Bus request.
 */
static bool cdbus_process_win_set(session_t *ps, DBusMessage *msg) {
	cdbus_window_t wid = XCB_NONE;
	const char *target = NULL;
	DBusError err = {};

	if (!dbus_message_get_args(msg, &err, CDBUS_TYPE_WINDOW, &wid, DBUS_TYPE_STRING,
	                           &target, DBUS_TYPE_INVALID)) {
		log_error("(): Failed to parse argument of \"win_set\" (%s).", err.message);
		dbus_error_free(&err);
		return false;
	}

	auto w = find_managed_win(ps, wid);

	if (!w) {
		log_error("Window %#010x not found.", wid);
		cdbus_reply_err(ps, msg, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return true;
	}

#define cdbus_m_win_set_do(tgt, type, real_type)                                         \
	if (!strcmp(MSTR(tgt), target)) {                                                \
		real_type val;                                                           \
		if (!cdbus_msg_get_arg(msg, 2, type, &val))                              \
			return false;                                                    \
		w->tgt = val;                                                            \
		goto cdbus_process_win_set_success;                                      \
	}

	if (!strcmp("shadow_force", target)) {
		cdbus_enum_t val = UNSET;
		if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
			return false;
		win_set_shadow_force(ps, w, val);
		goto cdbus_process_win_set_success;
	}

	if (!strcmp("fade_force", target)) {
		cdbus_enum_t val = UNSET;
		if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
			return false;
		win_set_fade_force(w, val);
		goto cdbus_process_win_set_success;
	}

	if (!strcmp("focused_force", target)) {
		cdbus_enum_t val = UNSET;
		if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
			return false;
		win_set_focused_force(ps, w, val);
		goto cdbus_process_win_set_success;
	}

	if (!strcmp("invert_color_force", target)) {
		cdbus_enum_t val = UNSET;
		if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
			return false;
		win_set_invert_color_force(ps, w, val);
		goto cdbus_process_win_set_success;
	}
#undef cdbus_m_win_set_do

	log_error(CDBUS_ERROR_BADTGT_S, target);
	cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

	return true;

cdbus_process_win_set_success:
	if (!dbus_message_get_no_reply(msg))
		cdbus_reply_bool(ps, msg, true);
	return true;
}

/**
 * Process a find_win D-Bus request.
 */
static bool cdbus_process_find_win(session_t *ps, DBusMessage *msg) {
	const char *target = NULL;

	if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target))
		return false;

	xcb_window_t wid = XCB_NONE;

	// Find window by client window
	if (!strcmp("client", target)) {
		cdbus_window_t client = XCB_NONE;
		if (!cdbus_msg_get_arg(msg, 1, CDBUS_TYPE_WINDOW, &client))
			return false;
		auto w = find_toplevel(ps, client);
		if (w) {
			wid = w->base.id;
		}
	}
	// Find focused window
	else if (!strcmp("focused", target)) {
		if (ps->active_win && ps->active_win->state != WSTATE_UNMAPPED) {
			wid = ps->active_win->base.id;
		}
	} else {
		log_error(CDBUS_ERROR_BADTGT_S, target);
		cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

		return true;
	}

	cdbus_reply_wid(ps, msg, wid);

	return true;
}

/**
 * Process a opts_get D-Bus request.
 */
static bool cdbus_process_opts_get(session_t *ps, DBusMessage *msg) {
	const char *target = NULL;

	if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target))
		return false;

#define cdbus_m_opts_get_do(tgt, apdarg_func)                                            \
	if (!strcmp(#tgt, target)) {                                                     \
		apdarg_func(ps, msg, ps->o.tgt);                                         \
		return true;                                                             \
	}

#define cdbus_m_opts_get_stub(tgt, apdarg_func, ret)                                     \
	if (!strcmp(#tgt, target)) {                                                     \
		apdarg_func(ps, msg, ret);                                               \
		return true;                                                             \
	}

	// version
	if (!strcmp("version", target)) {
		cdbus_reply_string(ps, msg, COMPTON_VERSION);
		return true;
	}

	// pid
	if (!strcmp("pid", target)) {
		cdbus_reply_int32(ps, msg, getpid());
		return true;
	}

	// display
	if (!strcmp("display", target)) {
		cdbus_reply_string(ps, msg, DisplayString(ps->dpy));
		return true;
	}

	cdbus_m_opts_get_stub(config_file, cdbus_reply_string, "Unknown");
	cdbus_m_opts_get_do(write_pid_path, cdbus_reply_string);
	cdbus_m_opts_get_do(mark_wmwin_focused, cdbus_reply_bool);
	cdbus_m_opts_get_do(mark_ovredir_focused, cdbus_reply_bool);
	cdbus_m_opts_get_do(detect_rounded_corners, cdbus_reply_bool);
	cdbus_m_opts_get_stub(paint_on_overlay, cdbus_reply_bool, ps->overlay != XCB_NONE);
	// paint_on_overlay_id: Get ID of the X composite overlay window
	if (!strcmp("paint_on_overlay_id", target)) {
		cdbus_reply_uint32(ps, msg, ps->overlay);
		return true;
	}
	cdbus_m_opts_get_do(unredir_if_possible, cdbus_reply_bool);
	cdbus_m_opts_get_do(unredir_if_possible_delay, cdbus_reply_int32l);
	cdbus_m_opts_get_do(redirected_force, cdbus_reply_enum);
	cdbus_m_opts_get_do(stoppaint_force, cdbus_reply_enum);
	cdbus_m_opts_get_do(logpath, cdbus_reply_string);

	cdbus_m_opts_get_do(refresh_rate, cdbus_reply_int32);
	cdbus_m_opts_get_do(sw_opti, cdbus_reply_bool);
	cdbus_m_opts_get_do(vsync, cdbus_reply_bool);
	if (!strcmp("backend", target)) {
		assert(ps->o.backend < sizeof(BACKEND_STRS) / sizeof(BACKEND_STRS[0]));
		cdbus_reply_string(ps, msg, BACKEND_STRS[ps->o.backend]);
		return true;
	}

	cdbus_m_opts_get_do(shadow_red, cdbus_reply_double);
	cdbus_m_opts_get_do(shadow_green, cdbus_reply_double);
	cdbus_m_opts_get_do(shadow_blue, cdbus_reply_double);
	cdbus_m_opts_get_do(shadow_radius, cdbus_reply_int32);
	cdbus_m_opts_get_do(shadow_offset_x, cdbus_reply_int32);
	cdbus_m_opts_get_do(shadow_offset_y, cdbus_reply_int32);
	cdbus_m_opts_get_do(shadow_opacity, cdbus_reply_double);
	cdbus_m_opts_get_do(xinerama_shadow_crop, cdbus_reply_bool);

	cdbus_m_opts_get_do(fade_delta, cdbus_reply_int32);
	cdbus_m_opts_get_do(fade_in_step, cdbus_reply_double);
	cdbus_m_opts_get_do(fade_out_step, cdbus_reply_double);
	cdbus_m_opts_get_do(no_fading_openclose, cdbus_reply_bool);

	cdbus_m_opts_get_do(blur_method, cdbus_reply_bool);
	cdbus_m_opts_get_do(blur_background_frame, cdbus_reply_bool);
	cdbus_m_opts_get_do(blur_background_fixed, cdbus_reply_bool);

	cdbus_m_opts_get_do(inactive_dim, cdbus_reply_double);
	cdbus_m_opts_get_do(inactive_dim_fixed, cdbus_reply_bool);

	cdbus_m_opts_get_do(use_ewmh_active_win, cdbus_reply_bool);
	cdbus_m_opts_get_do(detect_transient, cdbus_reply_bool);
	cdbus_m_opts_get_do(detect_client_leader, cdbus_reply_bool);

	cdbus_m_opts_get_do(use_damage, cdbus_reply_bool);

	cdbus_m_opts_get_stub(track_focus, cdbus_reply_bool, true);
	cdbus_m_opts_get_do(track_wdata, cdbus_reply_bool);
	cdbus_m_opts_get_do(track_leader, cdbus_reply_bool);
#undef cdbus_m_opts_get_do
#undef cdbus_m_opts_get_stub

	log_error(CDBUS_ERROR_BADTGT_S, target);
	cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

	return true;
}

// XXX Remove this after header clean up
void queue_redraw(session_t *ps);

/**
 * Process a opts_set D-Bus request.
 */
static bool cdbus_process_opts_set(session_t *ps, DBusMessage *msg) {
	const char *target = NULL;

	if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target))
		return false;

#define cdbus_m_opts_set_do(tgt, type, real_type)                                        \
	if (!strcmp(#tgt, target)) {                                                     \
		real_type val;                                                           \
		if (!cdbus_msg_get_arg(msg, 1, type, &val))                              \
			return false;                                                    \
		ps->o.tgt = val;                                                         \
		goto cdbus_process_opts_set_success;                                     \
	}

	// fade_delta
	if (!strcmp("fade_delta", target)) {
		int32_t val = 0;
		if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_INT32, &val)) {
			return false;
		}
		if (val <= 0) {
			return false;
		}
		ps->o.fade_delta = max2(val, 1);
		goto cdbus_process_opts_set_success;
	}

	// fade_in_step
	if (!strcmp("fade_in_step", target)) {
		double val = 0.0;
		if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_DOUBLE, &val))
			return false;
		ps->o.fade_in_step = normalize_d(val);
		goto cdbus_process_opts_set_success;
	}

	// fade_out_step
	if (!strcmp("fade_out_step", target)) {
		double val = 0.0;
		if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_DOUBLE, &val))
			return false;
		ps->o.fade_out_step = normalize_d(val);
		goto cdbus_process_opts_set_success;
	}

	// no_fading_openclose
	if (!strcmp("no_fading_openclose", target)) {
		dbus_bool_t val = FALSE;
		if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &val))
			return false;
		opts_set_no_fading_openclose(ps, val);
		goto cdbus_process_opts_set_success;
	}

	// unredir_if_possible
	if (!strcmp("unredir_if_possible", target)) {
		dbus_bool_t val = FALSE;
		if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &val))
			return false;
		if (ps->o.unredir_if_possible != val) {
			ps->o.unredir_if_possible = val;
			queue_redraw(ps);
		}
		goto cdbus_process_opts_set_success;
	}

	// clear_shadow
	if (!strcmp("clear_shadow", target)) {
		goto cdbus_process_opts_set_success;
	}

	// track_focus
	if (!strcmp("track_focus", target)) {
		goto cdbus_process_opts_set_success;
	}

	// redirected_force
	if (!strcmp("redirected_force", target)) {
		cdbus_enum_t val = UNSET;
		if (!cdbus_msg_get_arg(msg, 1, CDBUS_TYPE_ENUM, &val))
			return false;
		ps->o.redirected_force = val;
		force_repaint(ps);
		goto cdbus_process_opts_set_success;
	}

	// stoppaint_force
	cdbus_m_opts_set_do(stoppaint_force, CDBUS_TYPE_ENUM, cdbus_enum_t);

#undef cdbus_m_opts_set_do

	log_error(CDBUS_ERROR_BADTGT_S, target);
	cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

	return true;

cdbus_process_opts_set_success:
	if (!dbus_message_get_no_reply(msg))
		cdbus_reply_bool(ps, msg, true);
	return true;
}

/**
 * Process an Introspect D-Bus request.
 */
static bool cdbus_process_introspect(session_t *ps, DBusMessage *msg) {
	static const char *str_introspect =
	    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection "
	    "1.0//EN\"\n"
	    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	    "<node name='" CDBUS_OBJECT_NAME "'>\n"
	    "  <interface name='org.freedesktop.DBus.Introspectable'>\n"
	    "    <method name='Introspect'>\n"
	    "      <arg name='data' direction='out' type='s' />\n"
	    "    </method>\n"
	    "  </interface>\n"
	    "  <interface name='org.freedesktop.DBus.Peer'>\n"
	    "    <method name='Ping' />\n"
	    "    <method name='GetMachineId'>\n"
	    "      <arg name='machine_uuid' direction='out' type='s' />\n"
	    "    </method>\n"
	    "  </interface>\n"
	    "  <interface name='" CDBUS_INTERFACE_NAME "'>\n"
	    "    <signal name='win_added'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='win_destroyed'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='win_mapped'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='win_unmapped'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='win_focusin'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='win_focusout'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <method name='reset' />\n"
	    "    <method name='repaint' />\n"
	    "  </interface>\n"
	    "</node>\n";

	cdbus_reply_string(ps, msg, str_introspect);

	return true;
}
///@}

/**
 * Process a message from D-Bus.
 */
static DBusHandlerResult
cdbus_process(DBusConnection *c attr_unused, DBusMessage *msg, void *ud) {
	session_t *ps = ud;
	bool handled = false;

#define cdbus_m_ismethod(method)                                                         \
	dbus_message_is_method_call(msg, CDBUS_INTERFACE_NAME, method)

	if (cdbus_m_ismethod("reset")) {
		log_info("compton is resetting...");
		ev_break(ps->loop, EVBREAK_ALL);
		if (!dbus_message_get_no_reply(msg))
			cdbus_reply_bool(ps, msg, true);
		handled = true;
	} else if (cdbus_m_ismethod("repaint")) {
		force_repaint(ps);
		if (!dbus_message_get_no_reply(msg))
			cdbus_reply_bool(ps, msg, true);
		handled = true;
	} else if (cdbus_m_ismethod("list_win")) {
		handled = cdbus_process_list_win(ps, msg);
	} else if (cdbus_m_ismethod("win_get")) {
		handled = cdbus_process_win_get(ps, msg);
	} else if (cdbus_m_ismethod("win_set")) {
		handled = cdbus_process_win_set(ps, msg);
	} else if (cdbus_m_ismethod("find_win")) {
		handled = cdbus_process_find_win(ps, msg);
	} else if (cdbus_m_ismethod("opts_get")) {
		handled = cdbus_process_opts_get(ps, msg);
	} else if (cdbus_m_ismethod("opts_set")) {
		handled = cdbus_process_opts_set(ps, msg);
	}
#undef cdbus_m_ismethod
	else if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable",
	                                     "Introspect")) {
		handled = cdbus_process_introspect(ps, msg);
	} else if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Peer", "Ping")) {
		cdbus_reply(ps, msg, NULL, NULL);
		handled = true;
	} else if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Peer",
	                                       "GetMachineId")) {
		char *uuid = dbus_get_local_machine_id();
		if (uuid) {
			cdbus_reply_string(ps, msg, uuid);
			dbus_free(uuid);
			handled = true;
		}
	} else if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameAcquired") ||
	           dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameLost")) {
		handled = true;
	} else {
		if (DBUS_MESSAGE_TYPE_ERROR == dbus_message_get_type(msg)) {
			log_error(
			    "Error message of path \"%s\" "
			    "interface \"%s\", member \"%s\", error \"%s\"",
			    dbus_message_get_path(msg), dbus_message_get_interface(msg),
			    dbus_message_get_member(msg), dbus_message_get_error_name(msg));
		} else {
			log_error("Illegal message of type \"%s\", path \"%s\" "
			          "interface \"%s\", member \"%s\"",
			          cdbus_repr_msgtype(msg), dbus_message_get_path(msg),
			          dbus_message_get_interface(msg),
			          dbus_message_get_member(msg));
		}
		if (DBUS_MESSAGE_TYPE_METHOD_CALL == dbus_message_get_type(msg) &&
		    !dbus_message_get_no_reply(msg))
			cdbus_reply_err(ps, msg, CDBUS_ERROR_BADMSG, CDBUS_ERROR_BADMSG_S);
		handled = true;
	}

	// If the message could not be processed, and an reply is expected, return
	// an empty reply.
	if (!handled && DBUS_MESSAGE_TYPE_METHOD_CALL == dbus_message_get_type(msg) &&
	    !dbus_message_get_no_reply(msg)) {
		cdbus_reply_err(ps, msg, CDBUS_ERROR_UNKNOWN, CDBUS_ERROR_UNKNOWN_S);
		handled = true;
	}

	return handled ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** @name Core callbacks
 */
///@{
void cdbus_ev_win_added(session_t *ps, struct win *w) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn)
		cdbus_signal_wid(ps, "win_added", w->id);
}

void cdbus_ev_win_destroyed(session_t *ps, struct win *w) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn)
		cdbus_signal_wid(ps, "win_destroyed", w->id);
}

void cdbus_ev_win_mapped(session_t *ps, struct win *w) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn)
		cdbus_signal_wid(ps, "win_mapped", w->id);
}

void cdbus_ev_win_unmapped(session_t *ps, struct win *w) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn)
		cdbus_signal_wid(ps, "win_unmapped", w->id);
}

void cdbus_ev_win_focusout(session_t *ps, struct win *w) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn)
		cdbus_signal_wid(ps, "win_focusout", w->id);
}

void cdbus_ev_win_focusin(session_t *ps, struct win *w) {
	struct cdbus_data *cd = ps->dbus_data;
	if (cd->dbus_conn)
		cdbus_signal_wid(ps, "win_focusin", w->id);
}
//!@}
