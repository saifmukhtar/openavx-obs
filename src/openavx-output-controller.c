/*
OpenAVX for OBS Studio - Frontend Output Controller
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "openavx-output-controller.h"
#include "openavx-output.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <openavx.h>
#include <util/config-file.h>

#include <ctype.h>
#include <pthread.h>
#include <string.h>

#define OPENAVX_CONFIG_SECTION "OpenAVX"

#define DEFAULT_MAIN_NAME    "OpenAVX Program Output"
#define DEFAULT_PREVIEW_NAME "OpenAVX Preview Output"

enum {
	OPENAVX_CTRL_VC_UNCOMPRESSED = 0,
	OPENAVX_CTRL_VC_SPEEDHQ = 1,
	OPENAVX_CTRL_VC_H264 = 2,
	OPENAVX_CTRL_VC_HEVC = 3,
	OPENAVX_CTRL_VC_AV1 = 4,
};

struct controller_state {
	pthread_mutex_t lock;
	bool initialized;
	bool frontend_ready;
	bool exiting;

	struct openavx_output_config cfg;

	obs_output_t *main_output;
	char main_last_error[256];

	obs_view_t *preview_view;
	video_t *preview_video;
	obs_output_t *preview_output;
	char preview_last_error[256];
};

static struct controller_state g_ctrl = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.initialized = false,
	.frontend_ready = false,
	.exiting = false,
};

static void copy_str_bounded(char *dst, size_t cap, const char *src)
{
	if (!dst || cap == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t len = strnlen(src, cap - 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void trim_str_bounded(char *dst, size_t cap, const char *src)
{
	if (!dst || cap == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}

	const char *start = src;
	while (*start && isspace((unsigned char)*start))
		start++;

	size_t len = strlen(start);
	while (len > 0 && isspace((unsigned char)start[len - 1]))
		len--;

	if (len >= cap)
		len = cap - 1;
	memcpy(dst, start, len);
	dst[len] = '\0';
}

static int sanitize_video_compression(int vc)
{
	switch (vc) {
	case OPENAVX_CTRL_VC_SPEEDHQ:
	case OPENAVX_CTRL_VC_H264:
	case OPENAVX_CTRL_VC_HEVC:
	case OPENAVX_CTRL_VC_AV1:
		return vc;
	default:
		return OPENAVX_CTRL_VC_UNCOMPRESSED;
	}
}

static int clamp_speedhq_quality(int q)
{
	if (q < 20)
		return 20;
	if (q > 98)
		return 98;
	return q;
}

static int clamp_bitrate_kbps(int br)
{
	if (br < 1000)
		return 1000;
	if (br > 50000)
		return 50000;
	return br;
}

static int clamp_keyframe_interval(int gop)
{
	if (gop < 15)
		return 15;
	if (gop > 300)
		return 300;
	return gop;
}

static int sanitize_color_format(int cf)
{
	switch (cf) {
	case OAVX_COLOR_FORMAT_BGRX_BGRA:
	case OAVX_COLOR_FORMAT_UYVY_BGRA:
	case OAVX_COLOR_FORMAT_RGBX_RGBA:
	case OAVX_COLOR_FORMAT_UYVY_RGBA:
	case OAVX_COLOR_FORMAT_FASTEST:
	case OAVX_COLOR_FORMAT_BEST:
		return cf;
	default:
		return OAVX_COLOR_FORMAT_BEST;
	}
}

static int sanitize_audio_compression(int ac)
{
	switch (ac) {
	case OAVX_AUDIO_COMPRESSION_OFF:
	case OAVX_AUDIO_COMPRESSION_MANUAL:
	case OAVX_AUDIO_COMPRESSION_ADAPTIVE:
		return ac;
	default:
		return OAVX_AUDIO_COMPRESSION_OFF;
	}
}

static int sanitize_transport(int tr)
{
	switch (tr) {
	case OAVX_TRANSPORT_TCP:
	case OAVX_TRANSPORT_RUDP:
	case OAVX_TRANSPORT_UDP:
		return tr;
	default:
		return OAVX_TRANSPORT_TCP;
	}
}

void openavx_output_config_set_defaults(struct openavx_output_config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->main_enabled = false;
	copy_str_bounded(cfg->main_name, sizeof(cfg->main_name), DEFAULT_MAIN_NAME);
	cfg->main_groups[0] = '\0';

	cfg->preview_enabled = false;
	copy_str_bounded(cfg->preview_name, sizeof(cfg->preview_name), DEFAULT_PREVIEW_NAME);
	cfg->preview_groups[0] = '\0';
	cfg->preview_include_audio = false;

	cfg->video_compression = OPENAVX_CTRL_VC_UNCOMPRESSED;
	cfg->speedhq_quality = 96;
	cfg->bitrate_kbps = 6000;
	cfg->keyframe_interval = 60;
	cfg->passphrase[0] = '\0';
	cfg->bind_ip[0] = '\0';
	cfg->failover_target[0] = '\0';
	cfg->web_preview_enabled = false;
	cfg->web_preview_port = 8080;
	cfg->rtsp_enabled = false;
	cfg->rtsp_port = 8554;
	cfg->color_format = OAVX_COLOR_FORMAT_BEST;
	cfg->audio_compression = OAVX_AUDIO_COMPRESSION_OFF;
	cfg->transport = OAVX_TRANSPORT_TCP;
}

static void sanitize_config(struct openavx_output_config *cfg)
{
	char trimmed[256];

	trim_str_bounded(trimmed, sizeof(trimmed), cfg->main_name);
	copy_str_bounded(cfg->main_name, sizeof(cfg->main_name), trimmed[0] ? trimmed : DEFAULT_MAIN_NAME);

	trim_str_bounded(trimmed, sizeof(trimmed), cfg->main_groups);
	copy_str_bounded(cfg->main_groups, sizeof(cfg->main_groups), trimmed);

	trim_str_bounded(trimmed, sizeof(trimmed), cfg->preview_name);
	copy_str_bounded(cfg->preview_name, sizeof(cfg->preview_name), trimmed[0] ? trimmed : DEFAULT_PREVIEW_NAME);

	trim_str_bounded(trimmed, sizeof(trimmed), cfg->preview_groups);
	copy_str_bounded(cfg->preview_groups, sizeof(cfg->preview_groups), trimmed);

	cfg->video_compression = sanitize_video_compression(cfg->video_compression);
	cfg->speedhq_quality = clamp_speedhq_quality(cfg->speedhq_quality);
	cfg->bitrate_kbps = clamp_bitrate_kbps(cfg->bitrate_kbps);
	cfg->keyframe_interval = clamp_keyframe_interval(cfg->keyframe_interval);
	trim_str_bounded(trimmed, sizeof(trimmed), cfg->passphrase);
	copy_str_bounded(cfg->passphrase, sizeof(cfg->passphrase), trimmed);
	trim_str_bounded(trimmed, sizeof(trimmed), cfg->bind_ip);
	copy_str_bounded(cfg->bind_ip, sizeof(cfg->bind_ip), trimmed);
	trim_str_bounded(trimmed, sizeof(trimmed), cfg->failover_target);
	copy_str_bounded(cfg->failover_target, sizeof(cfg->failover_target), trimmed);
	cfg->color_format = sanitize_color_format(cfg->color_format);
	cfg->audio_compression = sanitize_audio_compression(cfg->audio_compression);
	cfg->transport = sanitize_transport(cfg->transport);
	if (cfg->web_preview_port < 1024 || cfg->web_preview_port > 65535)
		cfg->web_preview_port = 8080;
	if (cfg->rtsp_port < 1024 || cfg->rtsp_port > 65535)
		cfg->rtsp_port = 8554;
}

static void ensure_user_config_defaults(config_t *conf)
{
	if (!conf)
		return;
	config_set_default_bool(conf, OPENAVX_CONFIG_SECTION, "MainEnabled", false);
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "MainName", DEFAULT_MAIN_NAME);
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "MainGroups", "");

	config_set_default_bool(conf, OPENAVX_CONFIG_SECTION, "PreviewEnabled", false);
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "PreviewName", DEFAULT_PREVIEW_NAME);
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "PreviewGroups", "");
	config_set_default_bool(conf, OPENAVX_CONFIG_SECTION, "PreviewIncludeAudio", false);

	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "VideoCompression", OPENAVX_CTRL_VC_UNCOMPRESSED);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "SpeedHQQuality", 96);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "BitrateKbps", 6000);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "KeyframeInterval", 60);
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "Passphrase", "");
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "BindIP", "");
	config_set_default_string(conf, OPENAVX_CONFIG_SECTION, "FailoverTarget", "");
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "ColorFormat", OAVX_COLOR_FORMAT_BEST);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "AudioCompression", OAVX_AUDIO_COMPRESSION_OFF);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "Transport", OAVX_TRANSPORT_TCP);
	config_set_default_bool(conf, OPENAVX_CONFIG_SECTION, "WebPreviewEnabled", false);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "WebPreviewPort", 8080);
	config_set_default_bool(conf, OPENAVX_CONFIG_SECTION, "RtspEnabled", false);
	config_set_default_int(conf, OPENAVX_CONFIG_SECTION, "RtspPort", 8554);
}

static void load_config_from_disk(struct openavx_output_config *cfg)
{
	openavx_output_config_set_defaults(cfg);
	config_t *conf = obs_frontend_get_user_config();
	if (!conf)
		return;

	ensure_user_config_defaults(conf);

	cfg->main_enabled = config_get_bool(conf, OPENAVX_CONFIG_SECTION, "MainEnabled");
	copy_str_bounded(cfg->main_name, sizeof(cfg->main_name),
			 config_get_string(conf, OPENAVX_CONFIG_SECTION, "MainName"));
	copy_str_bounded(cfg->main_groups, sizeof(cfg->main_groups),
			 config_get_string(conf, OPENAVX_CONFIG_SECTION, "MainGroups"));

	cfg->preview_enabled = config_get_bool(conf, OPENAVX_CONFIG_SECTION, "PreviewEnabled");
	copy_str_bounded(cfg->preview_name, sizeof(cfg->preview_name),
			 config_get_string(conf, OPENAVX_CONFIG_SECTION, "PreviewName"));
	copy_str_bounded(cfg->preview_groups, sizeof(cfg->preview_groups),
			 config_get_string(conf, OPENAVX_CONFIG_SECTION, "PreviewGroups"));
	cfg->preview_include_audio = config_get_bool(conf, OPENAVX_CONFIG_SECTION, "PreviewIncludeAudio");

	cfg->video_compression = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "VideoCompression");
	cfg->speedhq_quality = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "SpeedHQQuality");
	cfg->bitrate_kbps = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "BitrateKbps");
	cfg->keyframe_interval = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "KeyframeInterval");
	copy_str_bounded(cfg->passphrase, sizeof(cfg->passphrase),
			 config_get_string(conf, OPENAVX_CONFIG_SECTION, "Passphrase"));
	copy_str_bounded(cfg->bind_ip, sizeof(cfg->bind_ip), config_get_string(conf, OPENAVX_CONFIG_SECTION, "BindIP"));
	copy_str_bounded(cfg->failover_target, sizeof(cfg->failover_target),
			 config_get_string(conf, OPENAVX_CONFIG_SECTION, "FailoverTarget"));
	cfg->color_format = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "ColorFormat");
	cfg->audio_compression = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "AudioCompression");
	cfg->transport = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "Transport");
	cfg->web_preview_enabled = config_get_bool(conf, OPENAVX_CONFIG_SECTION, "WebPreviewEnabled");
	cfg->web_preview_port = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "WebPreviewPort");
	cfg->rtsp_enabled = config_get_bool(conf, OPENAVX_CONFIG_SECTION, "RtspEnabled");
	cfg->rtsp_port = (int)config_get_int(conf, OPENAVX_CONFIG_SECTION, "RtspPort");

	sanitize_config(cfg);
}

static void save_config_to_disk(const struct openavx_output_config *cfg)
{
	config_t *conf = obs_frontend_get_user_config();
	if (!conf) {
		obs_log(LOG_WARNING, "[controller] obs_frontend_get_user_config returned NULL; settings not saved");
		return;
	}

	config_set_bool(conf, OPENAVX_CONFIG_SECTION, "MainEnabled", cfg->main_enabled);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "MainName", cfg->main_name);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "MainGroups", cfg->main_groups);

	config_set_bool(conf, OPENAVX_CONFIG_SECTION, "PreviewEnabled", cfg->preview_enabled);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "PreviewName", cfg->preview_name);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "PreviewGroups", cfg->preview_groups);
	config_set_bool(conf, OPENAVX_CONFIG_SECTION, "PreviewIncludeAudio", cfg->preview_include_audio);

	config_set_int(conf, OPENAVX_CONFIG_SECTION, "VideoCompression", cfg->video_compression);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "SpeedHQQuality", cfg->speedhq_quality);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "BitrateKbps", cfg->bitrate_kbps);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "KeyframeInterval", cfg->keyframe_interval);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "Passphrase", cfg->passphrase);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "BindIP", cfg->bind_ip);
	config_set_string(conf, OPENAVX_CONFIG_SECTION, "FailoverTarget", cfg->failover_target);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "ColorFormat", cfg->color_format);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "AudioCompression", cfg->audio_compression);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "Transport", cfg->transport);
	config_set_bool(conf, OPENAVX_CONFIG_SECTION, "WebPreviewEnabled", cfg->web_preview_enabled);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "WebPreviewPort", cfg->web_preview_port);
	config_set_bool(conf, OPENAVX_CONFIG_SECTION, "RtspEnabled", cfg->rtsp_enabled);
	config_set_int(conf, OPENAVX_CONFIG_SECTION, "RtspPort", cfg->rtsp_port);

	int rc = config_save_safe(conf, "tmp", NULL);
	if (rc != CONFIG_SUCCESS)
		obs_log(LOG_WARNING, "[controller] failed to save user config (rc=%d)", rc);
}

static obs_data_t *build_output_settings(const struct openavx_output_config *cfg, bool is_preview)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "stream_name", is_preview ? cfg->preview_name : cfg->main_name);
	obs_data_set_string(s, "groups", is_preview ? cfg->preview_groups : cfg->main_groups);
	obs_data_set_int(s, "video_compression", cfg->video_compression);
	obs_data_set_int(s, "speedhq_quality", cfg->speedhq_quality);
	obs_data_set_int(s, "bitrate_kbps", cfg->bitrate_kbps);
	obs_data_set_int(s, "keyframe_interval", cfg->keyframe_interval);
	obs_data_set_string(s, "passphrase", cfg->passphrase);
	obs_data_set_string(s, "bind_ip", cfg->bind_ip);
	obs_data_set_string(s, "failover_target", cfg->failover_target);
	obs_data_set_int(s, "color_format", cfg->color_format);
	obs_data_set_int(s, "audio_compression", cfg->audio_compression);
	obs_data_set_int(s, "transport", cfg->transport);
	obs_data_set_bool(s, "web_preview_enabled", !is_preview && cfg->web_preview_enabled);
	obs_data_set_int(s, "web_preview_port", cfg->web_preview_port);
	obs_data_set_bool(s, "rtsp_enabled", !is_preview && cfg->rtsp_enabled);
	obs_data_set_int(s, "rtsp_port", cfg->rtsp_port);
	return s;
}

static void stop_and_release_output(obs_output_t **out_ptr)
{
	obs_output_t *out = *out_ptr;
	if (!out)
		return;
	*out_ptr = NULL;

	if (obs_output_active(out))
		obs_output_stop(out);
	obs_output_release(out);
}

static void stop_main_output(void)
{
	obs_output_t *out = NULL;
	pthread_mutex_lock(&g_ctrl.lock);
	out = g_ctrl.main_output;
	g_ctrl.main_output = NULL;
	pthread_mutex_unlock(&g_ctrl.lock);

	stop_and_release_output(&out);
}

static void refresh_preview_view_source(void)
{
	pthread_mutex_lock(&g_ctrl.lock);
	obs_view_t *view = g_ctrl.preview_view;
	bool exiting = g_ctrl.exiting;
	pthread_mutex_unlock(&g_ctrl.lock);

	if (!view || exiting)
		return;

	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									 : obs_frontend_get_current_scene();
	obs_view_set_source(view, 0, scene);
	if (scene)
		obs_source_release(scene);
}

static void stop_preview_output(void)
{
	obs_output_t *out = NULL;
	obs_view_t *view = NULL;
	video_t *video = NULL;

	pthread_mutex_lock(&g_ctrl.lock);
	out = g_ctrl.preview_output;
	view = g_ctrl.preview_view;
	video = g_ctrl.preview_video;
	g_ctrl.preview_output = NULL;
	g_ctrl.preview_view = NULL;
	g_ctrl.preview_video = NULL;
	pthread_mutex_unlock(&g_ctrl.lock);

	stop_and_release_output(&out);

	if (view) {
		obs_view_set_source(view, 0, NULL);
		if (video)
			obs_view_remove(view);
		obs_view_destroy(view);
	}
}

static void stop_all_outputs_and_views(void)
{
	stop_main_output();
	stop_preview_output();
}

static bool shared_encoding_changed(const struct openavx_output_config *a, const struct openavx_output_config *b)
{
	return a->video_compression != b->video_compression || a->speedhq_quality != b->speedhq_quality ||
	       a->bitrate_kbps != b->bitrate_kbps || a->keyframe_interval != b->keyframe_interval ||
	       strcmp(a->passphrase, b->passphrase) != 0 || strcmp(a->bind_ip, b->bind_ip) != 0 ||
	       strcmp(a->failover_target, b->failover_target) != 0 || a->color_format != b->color_format ||
	       a->audio_compression != b->audio_compression || a->transport != b->transport;
}

static bool main_needs_restart(const struct openavx_output_config *prev, const struct openavx_output_config *next,
			       obs_output_t *cur, bool force_restart)
{
	if (!next->main_enabled)
		return cur != NULL;
	if (!cur || !obs_output_active(cur) || force_restart)
		return true;
	if (!prev->main_enabled)
		return true;
	if (strcmp(prev->main_name, next->main_name) != 0 || strcmp(prev->main_groups, next->main_groups) != 0 ||
	    prev->web_preview_enabled != next->web_preview_enabled ||
	    prev->web_preview_port != next->web_preview_port || prev->rtsp_enabled != next->rtsp_enabled ||
	    prev->rtsp_port != next->rtsp_port)
		return true;
	return shared_encoding_changed(prev, next);
}

static bool preview_needs_restart(const struct openavx_output_config *prev, const struct openavx_output_config *next,
				  obs_output_t *cur, bool force_restart)
{
	if (!next->preview_enabled)
		return cur != NULL;
	if (!cur || !obs_output_active(cur) || force_restart)
		return true;
	if (!prev->preview_enabled)
		return true;
	if (strcmp(prev->preview_name, next->preview_name) != 0 ||
	    strcmp(prev->preview_groups, next->preview_groups) != 0 ||
	    prev->preview_include_audio != next->preview_include_audio)
		return true;
	return shared_encoding_changed(prev, next);
}

static bool start_main_output(const struct openavx_output_config *cfg)
{
	video_t *video = obs_get_video();
	audio_t *audio = obs_get_audio();
	if (!video) {
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.main_last_error, sizeof(g_ctrl.main_last_error),
				 "OBS video subsystem is not active");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] cannot start Main Output: OBS video not active");
		return false;
	}

	obs_data_t *settings = build_output_settings(cfg, false);
	const char *output_id = audio ? "openavx_output" : "openavx_video_output";
	obs_output_t *out = obs_output_create(output_id, "OpenAVX Main Output", settings, NULL);
	obs_data_release(settings);

	if (!out) {
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.main_last_error, sizeof(g_ctrl.main_last_error),
				 "Failed to create OpenAVX Main Output");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] obs_output_create('%s') failed for Main Output", output_id);
		return false;
	}

	obs_output_set_media(out, video, audio);
	if (!obs_output_start(out)) {
		const char *err = obs_output_get_last_error(out);
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.main_last_error, sizeof(g_ctrl.main_last_error),
				 (err && err[0]) ? err : "Failed to start OpenAVX Main Output");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] Main Output start failed: %s",
			(err && err[0]) ? err : "unknown error");
		stop_and_release_output(&out);
		return false;
	}

	pthread_mutex_lock(&g_ctrl.lock);
	g_ctrl.main_output = out;
	g_ctrl.main_last_error[0] = '\0';
	pthread_mutex_unlock(&g_ctrl.lock);

	obs_log(LOG_INFO, "[controller] Main Output started ('%s')", cfg->main_name);
	return true;
}

static bool start_preview_output(const struct openavx_output_config *cfg)
{
	obs_view_t *view = obs_view_create();
	if (!view) {
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.preview_last_error, sizeof(g_ctrl.preview_last_error),
				 "Failed to create OBS Preview view");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] obs_view_create failed for Preview Output");
		return false;
	}

	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									 : obs_frontend_get_current_scene();
	obs_view_set_source(view, 0, scene);
	if (scene)
		obs_source_release(scene);

	video_t *video = obs_view_add(view);
	if (!video) {
		obs_view_set_source(view, 0, NULL);
		obs_view_destroy(view);
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.preview_last_error, sizeof(g_ctrl.preview_last_error),
				 "Failed to attach Preview view to OBS video mixer");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] obs_view_add failed for Preview Output");
		return false;
	}

	audio_t *audio = cfg->preview_include_audio ? obs_get_audio() : NULL;
	const char *output_id = audio ? "openavx_output" : "openavx_video_output";

	obs_data_t *settings = build_output_settings(cfg, true);
	obs_output_t *out = obs_output_create(output_id, "OpenAVX Preview Output", settings, NULL);
	obs_data_release(settings);

	if (!out) {
		obs_view_set_source(view, 0, NULL);
		obs_view_remove(view);
		obs_view_destroy(view);
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.preview_last_error, sizeof(g_ctrl.preview_last_error),
				 "Failed to create OpenAVX Preview Output");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] obs_output_create('%s') failed for Preview Output", output_id);
		return false;
	}

	obs_output_set_media(out, video, audio);
	if (!obs_output_start(out)) {
		const char *err = obs_output_get_last_error(out);
		pthread_mutex_lock(&g_ctrl.lock);
		copy_str_bounded(g_ctrl.preview_last_error, sizeof(g_ctrl.preview_last_error),
				 (err && err[0]) ? err : "Failed to start OpenAVX Preview Output");
		pthread_mutex_unlock(&g_ctrl.lock);
		obs_log(LOG_ERROR, "[controller] Preview Output start failed: %s",
			(err && err[0]) ? err : "unknown error");
		stop_and_release_output(&out);
		obs_view_set_source(view, 0, NULL);
		obs_view_remove(view);
		obs_view_destroy(view);
		return false;
	}

	pthread_mutex_lock(&g_ctrl.lock);
	g_ctrl.preview_view = view;
	g_ctrl.preview_video = video;
	g_ctrl.preview_output = out;
	g_ctrl.preview_last_error[0] = '\0';
	pthread_mutex_unlock(&g_ctrl.lock);

	obs_log(LOG_INFO, "[controller] Preview Output started ('%s', audio=%s)", cfg->preview_name,
		audio ? "on" : "off");
	return true;
}

static bool apply_outputs_for_config(const struct openavx_output_config *prev, const struct openavx_output_config *next,
				     bool force_restart)
{
	pthread_mutex_lock(&g_ctrl.lock);
	bool ready = g_ctrl.frontend_ready && !g_ctrl.exiting;
	obs_output_t *cur_main = g_ctrl.main_output;
	obs_output_t *cur_preview = g_ctrl.preview_output;
	pthread_mutex_unlock(&g_ctrl.lock);

	if (!ready)
		return true;

	bool ok = true;

	if (main_needs_restart(prev, next, cur_main, force_restart)) {
		stop_main_output();
		pthread_mutex_lock(&g_ctrl.lock);
		g_ctrl.main_last_error[0] = '\0';
		pthread_mutex_unlock(&g_ctrl.lock);
		if (next->main_enabled) {
			if (!start_main_output(next))
				ok = false;
		}
	}

	if (preview_needs_restart(prev, next, cur_preview, force_restart)) {
		stop_preview_output();
		pthread_mutex_lock(&g_ctrl.lock);
		g_ctrl.preview_last_error[0] = '\0';
		pthread_mutex_unlock(&g_ctrl.lock);
		if (next->preview_enabled) {
			if (!start_preview_output(next))
				ok = false;
		}
	}

	return ok;
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		struct openavx_output_config loaded;
		struct openavx_output_config prev;
		load_config_from_disk(&loaded);

		pthread_mutex_lock(&g_ctrl.lock);
		prev = g_ctrl.cfg;
		g_ctrl.cfg = loaded;
		g_ctrl.frontend_ready = true;
		g_ctrl.exiting = false;
		pthread_mutex_unlock(&g_ctrl.lock);

		apply_outputs_for_config(&prev, &loaded, false);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP: {
		pthread_mutex_lock(&g_ctrl.lock);
		obs_view_t *view = g_ctrl.preview_view;
		pthread_mutex_unlock(&g_ctrl.lock);
		if (view)
			obs_view_set_source(view, 0, NULL);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		refresh_preview_view_source();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_log(LOG_INFO, "[controller] OBS exiting; stopping OpenAVX outputs");
		pthread_mutex_lock(&g_ctrl.lock);
		g_ctrl.exiting = true;
		g_ctrl.frontend_ready = false;
		pthread_mutex_unlock(&g_ctrl.lock);
		stop_all_outputs_and_views();
		break;
	default:
		break;
	}
}

void openavx_output_controller_init(void)
{
	pthread_mutex_lock(&g_ctrl.lock);
	if (g_ctrl.initialized) {
		pthread_mutex_unlock(&g_ctrl.lock);
		return;
	}
	openavx_output_config_set_defaults(&g_ctrl.cfg);
	g_ctrl.frontend_ready = false;
	g_ctrl.exiting = false;
	g_ctrl.main_last_error[0] = '\0';
	g_ctrl.preview_last_error[0] = '\0';
	g_ctrl.initialized = true;
	pthread_mutex_unlock(&g_ctrl.lock);

	obs_frontend_add_event_callback(on_frontend_event, NULL);
}

void openavx_output_controller_shutdown(void)
{
	pthread_mutex_lock(&g_ctrl.lock);
	if (!g_ctrl.initialized) {
		pthread_mutex_unlock(&g_ctrl.lock);
		return;
	}
	g_ctrl.exiting = true;
	g_ctrl.frontend_ready = false;
	pthread_mutex_unlock(&g_ctrl.lock);

	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	stop_all_outputs_and_views();

	pthread_mutex_lock(&g_ctrl.lock);
	g_ctrl.initialized = false;
	pthread_mutex_unlock(&g_ctrl.lock);
}

bool openavx_output_controller_get_config(struct openavx_output_config *out_cfg)
{
	if (!out_cfg)
		return false;

	pthread_mutex_lock(&g_ctrl.lock);
	bool ready = g_ctrl.frontend_ready;
	*out_cfg = g_ctrl.cfg;
	pthread_mutex_unlock(&g_ctrl.lock);

	if (!ready)
		load_config_from_disk(out_cfg);
	return true;
}

bool openavx_output_controller_apply_config(const struct openavx_output_config *cfg, bool force_restart)
{
	if (!cfg)
		return false;

	struct openavx_output_config clean = *cfg;
	sanitize_config(&clean);

	pthread_mutex_lock(&g_ctrl.lock);
	struct openavx_output_config prev = g_ctrl.cfg;
	g_ctrl.cfg = clean;
	pthread_mutex_unlock(&g_ctrl.lock);

	save_config_to_disk(&clean);
	return apply_outputs_for_config(&prev, &clean, force_restart);
}

void openavx_output_controller_get_status(struct openavx_output_status *out_status)
{
	if (!out_status)
		return;
	memset(out_status, 0, sizeof(*out_status));

	pthread_mutex_lock(&g_ctrl.lock);
	out_status->frontend_ready = g_ctrl.frontend_ready && !g_ctrl.exiting;

	if (g_ctrl.main_output) {
		out_status->main_active = obs_output_active(g_ctrl.main_output);
		out_status->main_total_bytes = obs_output_get_total_bytes(g_ctrl.main_output);
		out_status->main_total_frames = obs_output_get_total_frames(g_ctrl.main_output);
		out_status->main_dropped_frames = obs_output_get_frames_dropped(g_ctrl.main_output);
		out_status->main_congestion = obs_output_get_congestion(g_ctrl.main_output);
		uint32_t tflags = openavx_output_get_tally_flags(g_ctrl.main_output);
		out_status->main_tally_program = (tflags & OAVX_TALLY_PROGRAM) != 0;
		out_status->main_tally_preview = (tflags & OAVX_TALLY_PREVIEW) != 0;
		out_status->main_clients = openavx_output_get_connection_count(g_ctrl.main_output);
		openavx_output_get_web_preview_url(g_ctrl.main_output, out_status->main_web_url,
						   sizeof(out_status->main_web_url));
		out_status->main_rtsp_port = openavx_output_get_rtsp_port(g_ctrl.main_output);
		const char *err = obs_output_get_last_error(g_ctrl.main_output);
		copy_str_bounded(out_status->main_last_error, sizeof(out_status->main_last_error),
				 (err && err[0]) ? err : g_ctrl.main_last_error);
	} else {
		copy_str_bounded(out_status->main_last_error, sizeof(out_status->main_last_error),
				 g_ctrl.main_last_error);
	}

	if (g_ctrl.preview_output) {
		out_status->preview_active = obs_output_active(g_ctrl.preview_output);
		out_status->preview_total_bytes = obs_output_get_total_bytes(g_ctrl.preview_output);
		out_status->preview_total_frames = obs_output_get_total_frames(g_ctrl.preview_output);
		out_status->preview_dropped_frames = obs_output_get_frames_dropped(g_ctrl.preview_output);
		out_status->preview_congestion = obs_output_get_congestion(g_ctrl.preview_output);
		uint32_t tflags = openavx_output_get_tally_flags(g_ctrl.preview_output);
		out_status->preview_tally_program = (tflags & OAVX_TALLY_PROGRAM) != 0;
		out_status->preview_tally_preview = (tflags & OAVX_TALLY_PREVIEW) != 0;
		out_status->preview_clients = openavx_output_get_connection_count(g_ctrl.preview_output);
		const char *err = obs_output_get_last_error(g_ctrl.preview_output);
		copy_str_bounded(out_status->preview_last_error, sizeof(out_status->preview_last_error),
				 (err && err[0]) ? err : g_ctrl.preview_last_error);
	} else {
		copy_str_bounded(out_status->preview_last_error, sizeof(out_status->preview_last_error),
				 g_ctrl.preview_last_error);
	}
	pthread_mutex_unlock(&g_ctrl.lock);
}
