// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <stdlib.h>
#include <string.h>

#include <xcb/randr.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/driver.h"
#include "common.h"
#include "compiler.h"
#include "log.h"

/// Apply driver specified global workarounds. It's safe to call this multiple times.
void apply_driver_workarounds(struct session *ps, enum driver driver) {
}

enum driver detect_driver(xcb_connection_t *c, backend_t *backend_data, xcb_window_t window) {
	enum driver ret = 0;
	// First we try doing backend agnostic detection using RANDR
	// There's no way to query the X server about what driver is loaded, so RANDR is
	// our best shot.
	auto randr_version = xcb_randr_query_version_reply(
	    c, xcb_randr_query_version(c, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION),
	    NULL);
	if (randr_version &&
	    (randr_version->major_version > 1 || randr_version->minor_version >= 4)) {
		auto r = xcb_randr_get_providers_reply(
		    c, xcb_randr_get_providers(c, window), NULL);
		if (r == NULL) {
			log_warn("Failed to get RANDR providers");
			free(randr_version);
			return 0;
		}

		auto providers = xcb_randr_get_providers_providers(r);
		for (auto i = 0; i < xcb_randr_get_providers_providers_length(r); i++) {
			auto r2 = xcb_randr_get_provider_info_reply(
			    c, xcb_randr_get_provider_info(c, providers[i], r->timestamp), NULL);
			if (r2 == NULL) {
				continue;
			}
			if (r2->num_outputs == 0) {
				free(r2);
				continue;
			}

			auto name_len = xcb_randr_get_provider_info_name_length(r2);
			assert(name_len >= 0);
			auto name =
			    strndup(xcb_randr_get_provider_info_name(r2), (size_t)name_len);
			if (strcasestr(name, "modesetting") != NULL) {
				ret |= DRIVER_MODESETTING;
			} else if (strcasestr(name, "Radeon") != NULL) {
				// Be conservative, add both radeon drivers
				ret |= DRIVER_AMDGPU | DRIVER_RADEON;
			} else if (strcasestr(name, "NVIDIA") != NULL) {
				ret |= DRIVER_NVIDIA;
			} else if (strcasestr(name, "nouveau") != NULL) {
				ret |= DRIVER_NOUVEAU;
			} else if (strcasestr(name, "Intel") != NULL) {
				ret |= DRIVER_INTEL;
			}
			free(name);
			free(r2);
		}
		free(r);
	}
	free(randr_version);

	// If the backend supports driver detection, use that as well
	if (backend_data && backend_data->ops->detect_driver) {
		ret |= backend_data->ops->detect_driver(backend_data);
	}
	return ret;
}
