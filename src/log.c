#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_OPENGL
#include <GL/gl.h>
#include "backend/gl/glx.h"
#include "backend/gl/gl_common.h"
#endif

#include "compiler.h"
#include "log.h"
#include "utils.h"

thread_local struct log *tls_logger;

struct log_target;

struct log {
	struct log_target *head;

	int log_level;
};

struct log_target {
	const struct log_ops *ops;
	struct log_target *next;
};

struct log_ops {
	void (*write)(struct log_target *, const char *, size_t);
	void (*writev)(struct log_target *, const struct iovec *, int vcnt);
	void (*destroy)(struct log_target *);

	/// Additional strings to print around the log_level string
	const char *(*colorize_begin)(enum log_level);
	const char *(*colorize_end)(enum log_level);
};

/// Fallback writev for targets don't implement it
static attr_unused void
log_default_writev(struct log_target *tgt, const struct iovec *vec, int vcnt) {
	size_t total = 0;
	for (int i = 0; i < vcnt; i++) {
		total += vec[i].iov_len;
	}

	if (!total) {
		// Nothing to write
		return;
	}
	char *buf = ccalloc(total, char);
	total = 0;
	for (int i = 0; i < vcnt; i++) {
		memcpy(buf + total, vec[i].iov_base, vec[i].iov_len);
		total += vec[i].iov_len;
	}
	tgt->ops->write(tgt, buf, total);
	free(buf);
}

static attr_const const char *log_level_to_string(enum log_level level) {
	switch (level) {
	case LOG_LEVEL_TRACE: return "TRACE";
	case LOG_LEVEL_DEBUG: return "DEBUG";
	case LOG_LEVEL_INFO: return "INFO";
	case LOG_LEVEL_WARN: return "WARN";
	case LOG_LEVEL_ERROR: return "ERROR";
	case LOG_LEVEL_FATAL: return "FATAL ERROR";
	default: return "????";
	}
}

enum log_level string_to_log_level(const char *str) {
	if (strcasecmp(str, "TRACE") == 0)
		return LOG_LEVEL_TRACE;
	else if (strcasecmp(str, "DEBUG") == 0)
		return LOG_LEVEL_DEBUG;
	else if (strcasecmp(str, "INFO") == 0)
		return LOG_LEVEL_INFO;
	else if (strcasecmp(str, "WARN") == 0)
		return LOG_LEVEL_WARN;
	else if (strcasecmp(str, "ERROR") == 0)
		return LOG_LEVEL_ERROR;
	return LOG_LEVEL_INVALID;
}

struct log *log_new(void) {
	auto ret = cmalloc(struct log);
	ret->log_level = LOG_LEVEL_WARN;
	ret->head = NULL;
	return ret;
}

void log_add_target(struct log *l, struct log_target *tgt) {
	assert(tgt->ops->writev);
	tgt->next = l->head;
	l->head = tgt;
}

/// Remove a previously added log target for a log struct, and destroy it. If the log
/// target was never added, nothing happens.
void log_remove_target(struct log *l, struct log_target *tgt) {
	struct log_target *now = l->head, **prev = &l->head;
	while (now) {
		if (now == tgt) {
			*prev = now->next;
			tgt->ops->destroy(tgt);
			break;
		}
		prev = &now->next;
		now = now->next;
	}
}

/// Destroy a log struct and every log target added to it
void log_destroy(struct log *l) {
	// free all tgt
	struct log_target *head = l->head;
	while (head) {
		auto next = head->next;
		head->ops->destroy(head);
		head = next;
	}
	free(l);
}

void log_set_level(struct log *l, int level) {
	assert(level <= LOG_LEVEL_FATAL && level >= 0);
	l->log_level = level;
}

enum log_level log_get_level(const struct log *l) {
	return l->log_level;
}

attr_printf(4, 5) void log_printf(struct log *l, int level, const char *func,
                                  const char *fmt, ...) {
	assert(level <= LOG_LEVEL_FATAL && level >= 0);
	if (level < l->log_level)
		return;

	char *buf = NULL;
	va_list args;

	va_start(args, fmt);
	int blen = vasprintf(&buf, fmt, args);
	va_end(args);

	if (blen < 0 || !buf) {
		free(buf);
		return;
	}

	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	auto tm = localtime(&ts.tv_sec);
	char time_buf[100];
	strftime(time_buf, sizeof time_buf, "%x %T", tm);

	char *time = NULL;
	int tlen = asprintf(&time, "%s.%03ld", time_buf, ts.tv_nsec / 1000000);
	if (tlen < 0 || !time) {
		free(buf);
		free(time);
		return;
	}

	const char *log_level_str = log_level_to_string(level);
	size_t llen = strlen(log_level_str);
	size_t flen = strlen(func);

	struct log_target *head = l->head;
	while (head) {
		const char *p = "", *s = "";
		size_t plen = 0, slen = 0;

		if (head->ops->colorize_begin) {
			// construct target specific prefix
			p = head->ops->colorize_begin(level);
			plen = strlen(p);
			if (head->ops->colorize_end) {
				s = head->ops->colorize_end(level);
				slen = strlen(s);
			}
		}
		head->ops->writev(
		    head,
		    (struct iovec[]){{.iov_base = "[ ", .iov_len = 2},
		                     {.iov_base = time, .iov_len = (size_t)tlen},
		                     {.iov_base = " ", .iov_len = 1},
		                     {.iov_base = (void *)func, .iov_len = flen},
		                     {.iov_base = " ", .iov_len = 1},
		                     {.iov_base = (void *)p, .iov_len = plen},
		                     {.iov_base = (void *)log_level_str, .iov_len = llen},
		                     {.iov_base = (void *)s, .iov_len = slen},
		                     {.iov_base = " ] ", .iov_len = 3},
		                     {.iov_base = buf, .iov_len = (size_t)blen},
		                     {.iov_base = "\n", .iov_len = 1}},
		    11);
		head = head->next;
	}
	free(time);
	free(buf);
}

/// A trivial deinitializer that simply frees the memory
static attr_unused void logger_trivial_destroy(struct log_target *tgt) {
	free(tgt);
}

/// A null log target that does nothing
static const struct log_ops null_logger_ops;
static struct log_target null_logger_target = {
    .ops = &null_logger_ops,
};

struct log_target *null_logger_new(void) {
	return &null_logger_target;
}

static void null_logger_write(struct log_target *tgt attr_unused,
                              const char *str attr_unused, size_t len attr_unused) {
	return;
}

static void null_logger_writev(struct log_target *tgt attr_unused,
                               const struct iovec *vec attr_unused, int vcnt attr_unused) {
	return;
}

static const struct log_ops null_logger_ops = {
    .write = null_logger_write,
    .writev = null_logger_writev,
};

/// A file based logger that writes to file (or stdout/stderr)
struct file_logger {
	struct log_target tgt;
	FILE *f;
	struct log_ops ops;
};

static void file_logger_write(struct log_target *tgt, const char *str, size_t len) {
	auto f = (struct file_logger *)tgt;
	fwrite(str, 1, len, f->f);
}

static void file_logger_writev(struct log_target *tgt, const struct iovec *vec, int vcnt) {
	auto f = (struct file_logger *)tgt;
	fflush(f->f);
	writev(fileno(f->f), vec, vcnt);
}

static void file_logger_destroy(struct log_target *tgt) {
	auto f = (struct file_logger *)tgt;
	fclose(f->f);
	free(tgt);
}

#define ANSI(x) "\033[" x "m"
static const char *terminal_colorize_begin(enum log_level level) {
	switch (level) {
	case LOG_LEVEL_TRACE: return ANSI("30;2");
	case LOG_LEVEL_DEBUG: return ANSI("37;2");
	case LOG_LEVEL_INFO: return ANSI("92");
	case LOG_LEVEL_WARN: return ANSI("33");
	case LOG_LEVEL_ERROR: return ANSI("31;1");
	case LOG_LEVEL_FATAL: return ANSI("30;103;1");
	default: return "";
	}
}

static const char *terminal_colorize_end(enum log_level level attr_unused) {
	return ANSI("0");
}
#undef PREFIX

static const struct log_ops file_logger_ops = {
    .write = file_logger_write,
    .writev = file_logger_writev,
    .destroy = file_logger_destroy,
};

struct log_target *file_logger_new(const char *filename) {
	FILE *f = fopen(filename, "a");
	if (!f) {
		return NULL;
	}

	auto ret = cmalloc(struct file_logger);
	ret->tgt.ops = &ret->ops;
	ret->f = f;

	// Always assume a file is not a terminal
	ret->ops = file_logger_ops;

	return &ret->tgt;
}

struct log_target *stderr_logger_new(void) {
	int fd = dup(STDERR_FILENO);
	if (fd < 0) {
		return NULL;
	}

	FILE *f = fdopen(fd, "w");
	if (!f) {
		return NULL;
	}

	auto ret = cmalloc(struct file_logger);
	ret->tgt.ops = &ret->ops;
	ret->f = f;
	ret->ops = file_logger_ops;

	if (isatty(fd)) {
		ret->ops.colorize_begin = terminal_colorize_begin;
		ret->ops.colorize_end = terminal_colorize_end;
	}
	return &ret->tgt;
}

// TODO: move this to gl_common.c
#ifdef CONFIG_OPENGL
/// An opengl logger that can be used for logging into opengl debugging tools,
/// such as apitrace
struct gl_string_marker_logger {
	struct log_target tgt;
	PFNGLSTRINGMARKERGREMEDYPROC gl_string_marker;
};

static void gl_string_marker_logger_write(struct log_target *tgt, const char *str, size_t len) {
	auto g = (struct gl_string_marker_logger *)tgt;
	g->gl_string_marker((GLsizei)len, str);
}

static const struct log_ops gl_string_marker_logger_ops = {
    .write = gl_string_marker_logger_write,
    .writev = log_default_writev,
    .destroy = logger_trivial_destroy,
};

struct log_target *gl_string_marker_logger_new(void) {
	if (!gl_has_extension("GL_GREMEDY_string_marker")) {
		return NULL;
	}

	void *fnptr = glXGetProcAddress((GLubyte *)"glStringMarkerGREMEDY");
	if (!fnptr)
		return NULL;

	auto ret = cmalloc(struct gl_string_marker_logger);
	ret->tgt.ops = &gl_string_marker_logger_ops;
	ret->gl_string_marker = fnptr;
	return &ret->tgt;
}

#else
struct log_target *gl_string_marker_logger_new(void) {
	return NULL;
}
#endif

// vim: set noet sw=8 ts=8:
