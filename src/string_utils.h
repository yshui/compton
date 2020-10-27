// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <ctype.h>
#include <stddef.h>

#include "compiler.h"

#define mstrncmp(s1, s2) strncmp((s1), (s2), strlen(s1))

char *mstrjoin(const char *src1, const char *src2);
void mstrextend(char **psrc1, const char *src2);

/// Parse a floating point number of form (+|-)?[0-9]*(\.[0-9]*)
double strtod_simple(const char *, const char **);

static inline int uitostr(unsigned int n, char *buf) {
	int ret = 0;
	unsigned int tmp = n;
	while (tmp > 0) {
		tmp /= 10;
		ret++;
	}

	if (ret == 0)
		ret = 1;

	int pos = ret;
	while (pos--) {
		buf[pos] = (char)(n % 10 + '0');
		n /= 10;
	}
	return ret;
}

static inline const char *skip_space_const(const char *src) {
	if (!src)
		return NULL;
	while (*src && isspace(*src))
		src++;
	return src;
}

static inline char *skip_space_mut(char *src) {
	if (!src)
		return NULL;
	while (*src && isspace(*src))
		src++;
	return src;
}

#define skip_space(x)                                                                    \
	_Generic((x), char * : skip_space_mut, const char * : skip_space_const)(x)
