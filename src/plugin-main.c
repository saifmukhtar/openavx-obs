/*
OpenAVX for OBS Studio
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <openavx.h>
#include "openavx-source.h"
#include "openavx-output.h"
#include "openavx-filter.h"
#ifdef ENABLE_FRONTEND_API
#include "openavx-output-controller.h"
#endif
#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)
#include "openavx-output-dialog.hpp"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void openavx_obs_log_cb(oavx_log_level_e level, const char *module, const char *message, void *user_ctx)
{
	(void)user_ctx;
	int obs_level = LOG_INFO;
	switch (level) {
	case OAVX_LOG_ERROR:
		obs_level = LOG_ERROR;
		break;
	case OAVX_LOG_WARN:
		obs_level = LOG_WARNING;
		break;
	case OAVX_LOG_DEBUG:
		obs_level = LOG_DEBUG;
		break;
	case OAVX_LOG_QUIET:
		return;
	case OAVX_LOG_INFO:
	default:
		obs_level = LOG_INFO;
		break;
	}
	obs_log(obs_level, "[core/%s] %s", module ? module : "avx", message ? message : "");
}

bool obs_module_load(void)
{
	oavx_set_log_callback(openavx_obs_log_cb, NULL);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_log(LOG_INFO, "linked against core engine: %s", oavx_version());

	openavx_source_global_init();

	obs_register_source(&openavx_source_info);
	obs_log(LOG_INFO, "registered 'openavx_source' input source");

	obs_register_source(&openavx_filter_info);
	obs_register_source(&openavx_audio_filter_info);
	obs_log(LOG_INFO, "registered 'openavx_filter' and 'openavx_audio_filter' ISO filters");

	obs_register_output(&openavx_output_info);
	obs_register_output(&openavx_video_output_info);
	obs_log(LOG_INFO, "registered 'openavx_output' and 'openavx_video_output' backends");

#ifdef ENABLE_FRONTEND_API
	openavx_output_controller_init();
#ifdef ENABLE_QT
	openavx_output_ui_init();
	obs_log(LOG_INFO, "registered Tools -> OpenAVX Output Settings menu item");
#endif
#endif

	return true;
}

void obs_module_unload(void)
{
#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)
	openavx_output_ui_shutdown();
#endif
#ifdef ENABLE_FRONTEND_API
	openavx_output_controller_shutdown();
#endif
	openavx_source_global_shutdown();
	oavx_set_log_callback(NULL, NULL);
	obs_log(LOG_INFO, "plugin unloaded");
}
