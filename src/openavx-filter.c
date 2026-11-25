/*
OpenAVX for OBS Studio - Dedicated ISO Output Filter
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "openavx-filter.h"
#include "openavx-source.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <openavx.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <util/threading.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_FILTER_NAME         obs_module_text("OpenAVX.Filter.Name")
#define TEXT_AUDIO_FILTER_NAME   obs_module_text("OpenAVX.Filter.AudioName")
#define TEXT_ACTIVE              obs_module_text("OpenAVX.Filter.Active")
#define TEXT_STREAM_NAME         obs_module_text("OpenAVX.Output.StreamName")
#define TEXT_GROUPS              obs_module_text("OpenAVX.Output.Groups")
#define TEXT_PASSPHRASE          obs_module_text("OpenAVX.Output.Passphrase")
#define TEXT_BIND_IP             obs_module_text("OpenAVX.Output.BindIP")
#define TEXT_FAILOVER_TARGET     obs_module_text("OpenAVX.Output.FailoverTarget")
#define TEXT_VIDEO_COMPRESSION   obs_module_text("OpenAVX.Output.VideoCompression")
#define TEXT_VC_UNCOMPRESSED     obs_module_text("OpenAVX.Output.VideoCompression.Uncompressed")
#define TEXT_VC_SPEEDHQ          obs_module_text("OpenAVX.Output.VideoCompression.SpeedHQ")
#define TEXT_VC_H264             obs_module_text("OpenAVX.Output.VideoCompression.H264")
#define TEXT_VC_HEVC             obs_module_text("OpenAVX.Output.VideoCompression.HEVC")
#define TEXT_VC_AV1              obs_module_text("OpenAVX.Output.VideoCompression.AV1")
#define TEXT_SPEEDHQ_QUALITY     obs_module_text("OpenAVX.Output.SpeedHQQuality")
#define TEXT_BITRATE             obs_module_text("OpenAVX.Output.Bitrate")
#define TEXT_KEYFRAME_INTERVAL   obs_module_text("OpenAVX.Output.KeyframeInterval")
#define TEXT_AUDIO_COMPRESSION   obs_module_text("OpenAVX.Output.AudioCompression")
#define TEXT_AC_OFF              obs_module_text("OpenAVX.Output.AudioCompression.Off")
#define TEXT_AC_ADAPTIVE         obs_module_text("OpenAVX.Output.AudioCompression.Adaptive")
#define TEXT_AC_MANUAL           obs_module_text("OpenAVX.Output.AudioCompression.Manual")
#define TEXT_TRANSPORT           obs_module_text("OpenAVX.Output.Transport")
#define TEXT_TRANSPORT_TCP       obs_module_text("OpenAVX.Output.Transport.TCP")
#define TEXT_TRANSPORT_RUDP      obs_module_text("OpenAVX.Output.Transport.RUDP")
#define TEXT_TRANSPORT_UDP       obs_module_text("OpenAVX.Output.Transport.UDP")
#define TEXT_STATUS              obs_module_text("OpenAVX.Filter.Status")
#define TEXT_GROUP_GENERAL       obs_module_text("OpenAVX.Filter.Group.General")
#define TEXT_GROUP_ENCODING      obs_module_text("OpenAVX.Filter.Group.Encoding")
#define TEXT_GROUP_NETWORK       obs_module_text("OpenAVX.Filter.Group.Network")

enum openavx_filter_video_compression {
	OPENAVX_VC_UNCOMPRESSED = 0,
	OPENAVX_VC_SPEEDHQ = 1,
	OPENAVX_VC_H264 = 2,
	OPENAVX_VC_HEVC = 3,
	OPENAVX_VC_AV1 = 4,
};

struct openavx_filter {
	obs_source_t *source;
	obs_source_t *target_source;
	oavx_context_t *ctx;
	oavx_stream_t *stream;
	pthread_mutex_t lock;

	/* Configuration */
	bool active;
	bool is_audio_only;
	char *stream_name;
	char *groups;
	char *passphrase;
	char *bind_ip;
	char *failover_target;
	int video_compression;
	int speedhq_quality;
	uint32_t bitrate_kbps;
	uint32_t keyframe_interval;
	int audio_compression;
	int transport;

	/* Offscreen graphics rendering and double-buffered DMA staging */
	uint32_t cx;
	uint32_t cy;
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurf[2];
	bool staged[2];
	int stage_idx;

	/* Reusable conversion buffers for SpeedHQ UYVA encoding */
	uint8_t *uyvy_buf;
	size_t uyvy_cap;
	uint8_t *alpha_buf;
	size_t alpha_cap;

	/* Telemetry & Counters */
	uint64_t video_frames_sent;
	uint64_t audio_frames_sent;
	uint64_t last_wire_bytes;
	long local_dropped_frames;
	uint32_t tally_flags;
	float current_bitrate_mbps;
	float current_rtt_ms;
	char status_text[320];

	/* Audio capture callback state */
	bool audio_callback_registered;
	uint32_t sample_rate;
	size_t channels;
};

static inline uint8_t clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

static inline int clamp_speedhq_quality(int q)
{
	if (q < 1)
		return 1;
	if (q > 100)
		return 100;
	return q;
}

static inline oavx_audio_compression_mode_e sanitize_audio_compression(int ac)
{
	switch (ac) {
	case OAVX_AUDIO_COMPRESSION_ADAPTIVE:
	case OAVX_AUDIO_COMPRESSION_MANUAL:
		return (oavx_audio_compression_mode_e)ac;
	default:
		return OAVX_AUDIO_COMPRESSION_OFF;
	}
}

static inline oavx_transport_e sanitize_transport(int tr)
{
	switch (tr) {
	case OAVX_TRANSPORT_RUDP:
	case OAVX_TRANSPORT_UDP:
		return (oavx_transport_e)tr;
	default:
		return OAVX_TRANSPORT_TCP;
	}
}

static bool ensure_filter_staging_buffers(struct openavx_filter *filter, size_t uyvy_needed, size_t alpha_needed)
{
	if (uyvy_needed > filter->uyvy_cap) {
		uint8_t *nb = (uint8_t *)brealloc(filter->uyvy_buf, uyvy_needed);
		if (!nb)
			return false;
		filter->uyvy_buf = nb;
		filter->uyvy_cap = uyvy_needed;
	}

	if (alpha_needed > filter->alpha_cap) {
		uint8_t *nb = (uint8_t *)brealloc(filter->alpha_buf, alpha_needed);
		if (!nb)
			return false;
		filter->alpha_buf = nb;
		filter->alpha_cap = alpha_needed;
	}

	return true;
}

static void convert_rgba_to_uyva(const uint8_t *src, uint32_t src_stride, uint32_t width, uint32_t height,
				 uint8_t *uyvy_dst, uint8_t *alpha_dst)
{
	size_t uyvy_row_bytes = (size_t)width * 2;

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *s = src + (size_t)y * src_stride;
		uint8_t *d = uyvy_dst + (size_t)y * uyvy_row_bytes;
		uint8_t *a = alpha_dst ? (alpha_dst + (size_t)y * width) : NULL;

		for (uint32_t x = 0; x + 1 < width; x += 2) {
			const uint8_t *p0 = s + (size_t)x * 4;
			const uint8_t *p1 = p0 + 4;

			int r0 = p0[0];
			int g0 = p0[1];
			int b0 = p0[2];

			int r1 = p1[0];
			int g1 = p1[1];
			int b1 = p1[2];

			int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
			int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;

			int r_avg = (r0 + r1) >> 1;
			int g_avg = (g0 + g1) >> 1;
			int b_avg = (b0 + b1) >> 1;

			int u = ((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128;
			int v = ((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128;

			size_t off = (size_t)x * 2;
			d[off + 0] = clamp_u8(u);
			d[off + 1] = clamp_u8(y0);
			d[off + 2] = clamp_u8(v);
			d[off + 3] = clamp_u8(y1);

			if (a) {
				a[x + 0] = p0[3];
				a[x + 1] = p1[3];
			}
		}

		if ((width & 1U) != 0U) {
			uint32_t x = width - 1;
			const uint8_t *p0 = s + (size_t)x * 4;
			int r0 = p0[0];
			int g0 = p0[1];
			int b0 = p0[2];

			int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
			int u = ((-38 * r0 - 74 * g0 + 112 * b0 + 128) >> 8) + 128;
			int v = ((112 * r0 - 94 * g0 - 18 * b0 + 128) >> 8) + 128;

			size_t off = (size_t)x * 2;
			d[off + 0] = clamp_u8(u);
			d[off + 1] = clamp_u8(y0);
			d[off + 2] = clamp_u8(v);
			d[off + 3] = clamp_u8(y0);
			if (a)
				a[x] = p0[3];
		}
	}
}

static void on_filter_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(source);
	struct openavx_filter *filter = (struct openavx_filter *)param;
	if (!filter || !audio_data || !audio_data->data[0] || audio_data->frames == 0)
		return;

	pthread_mutex_lock(&filter->lock);
	if (!filter->active || !filter->stream) {
		pthread_mutex_unlock(&filter->lock);
		return;
	}

	size_t ch = filter->channels ? filter->channels : 2;
	uint32_t rate = filter->sample_rate ? filter->sample_rate : 48000;

	oavx_audio_frame_t af;
	memset(&af, 0, sizeof(af));
	af.sample_rate = rate;
	af.channels = (uint16_t)ch;
	af.frames = audio_data->frames;
	af.format = OAVX_AUDIO_FMT_FLT_PLANAR;
	af.stride_bytes = (size_t)audio_data->frames * sizeof(float);
	af.pts_ns = (int64_t)audio_data->timestamp;

	if (muted) {
		/* When muted, send silence planes to keep receiver audio clock sync locked */
		af.planes[0] = NULL;
	} else {
		for (size_t i = 0; i < ch && i < 8; ++i) {
			af.planes[i] = audio_data->data[i];
		}
	}

	if (oavx_stream_send_audio(filter->stream, &af) == OAVX_SUCCESS)
		filter->audio_frames_sent++;

	pthread_mutex_unlock(&filter->lock);
}

static void openavx_filter_stop_stream_locked(struct openavx_filter *filter)
{
	if (!filter->stream)
		return;

	oavx_stream_close(filter->stream);
	filter->stream = NULL;

	obs_log(LOG_INFO, "[filter] stopped stream (video=%" PRIu64 ", audio=%" PRIu64 ", wire_bytes=%" PRIu64 ")",
		filter->video_frames_sent, filter->audio_frames_sent, filter->last_wire_bytes);
}

static bool openavx_filter_start_stream_locked(struct openavx_filter *filter)
{
	if (filter->stream)
		openavx_filter_stop_stream_locked(filter);

	obs_source_t *target = obs_filter_get_target(filter->source);
	const char *target_name = target ? obs_source_get_name(target) : "Source";

	char resolved_name[256];
	if (filter->stream_name && filter->stream_name[0] != '\0') {
		const char *placeholder = strstr(filter->stream_name, "${source}");
		if (!placeholder)
			placeholder = strstr(filter->stream_name, "<source>");

		if (placeholder) {
			size_t prefix_len = (size_t)(placeholder - filter->stream_name);
			size_t token_len = (placeholder[0] == '$') ? 9 : 8;
			const char *suffix = placeholder + token_len;
			snprintf(resolved_name, sizeof(resolved_name), "%.*s%s%s", (int)prefix_len, filter->stream_name,
				 target_name, suffix);
		} else {
			snprintf(resolved_name, sizeof(resolved_name), "%.255s", filter->stream_name);
		}
	} else {
		snprintf(resolved_name, sizeof(resolved_name), "OpenAVX ISO - %s", target_name);
	}

	const char *groups = (filter->groups && filter->groups[0] != '\0') ? filter->groups : NULL;
	const char *passphrase = (filter->passphrase && filter->passphrase[0] != '\0') ? filter->passphrase : NULL;
	const char *bind_ip = (filter->bind_ip && filter->bind_ip[0] != '\0') ? filter->bind_ip : NULL;

	oavx_stream_config_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.name = resolved_name;
	cfg.groups = groups;
	cfg.direction = OAVX_DIR_TRANSMIT;
	cfg.bandwidth = filter->is_audio_only ? OAVX_BANDWIDTH_AUDIO_ONLY : OAVX_BANDWIDTH_HIGHEST;
	cfg.color_format = OAVX_COLOR_FORMAT_BEST;
	cfg.transport = sanitize_transport(filter->transport);
	cfg.clock_sync = false;
	cfg.bind_ip = bind_ip;
	cfg.passphrase = passphrase;

	if (!filter->is_audio_only) {
		if (filter->video_compression == OPENAVX_VC_H264) {
			cfg.video_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
			cfg.video_bitrate_kbps = filter->bitrate_kbps ? filter->bitrate_kbps : 6000;
			cfg.keyframe_interval = filter->keyframe_interval ? filter->keyframe_interval : 60;
		} else if (filter->video_compression == OPENAVX_VC_HEVC) {
			cfg.video_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_HEVC;
			cfg.video_bitrate_kbps = filter->bitrate_kbps ? filter->bitrate_kbps : 6000;
			cfg.keyframe_interval = filter->keyframe_interval ? filter->keyframe_interval : 60;
		} else if (filter->video_compression == OPENAVX_VC_AV1) {
			cfg.video_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_AV01;
			cfg.video_bitrate_kbps = filter->bitrate_kbps ? filter->bitrate_kbps : 6000;
			cfg.keyframe_interval = filter->keyframe_interval ? filter->keyframe_interval : 60;
		}
	}

	oavx_speedhq_set_quality((uint8_t)filter->speedhq_quality);

	filter->stream = oavx_stream_open(filter->ctx, &cfg);
	if (!filter->stream) {
		obs_log(LOG_ERROR, "[filter] failed to open transmission stream '%s'", resolved_name);
		return false;
	}

	oavx_audio_compression_mode_e ac = sanitize_audio_compression(filter->audio_compression);
	if (ac != OAVX_AUDIO_COMPRESSION_OFF)
		oavx_stream_set_audio_compression(filter->stream, ac, 128);

	if (filter->failover_target && filter->failover_target[0] != '\0') {
		char fo_name[256] = {0};
		char fo_addr[256] = {0};
		if (openavx_source_resolve_endpoint(filter->failover_target, fo_name, sizeof(fo_name), fo_addr,
						    sizeof(fo_addr))) {
			oavx_endpoint_t fep;
			memset(&fep, 0, sizeof(fep));
			fep.name = fo_name[0] ? fo_name : filter->failover_target;
			fep.address = fo_addr;
			oavx_stream_set_failover(filter->stream, &fep);
			obs_log(LOG_INFO, "[filter] advertising failover target '%s' (%s)", fep.name, fep.address);
		} else {
			obs_log(LOG_WARNING, "[filter] could not resolve failover target '%s' to ip:port",
				filter->failover_target);
		}
	}

	filter->video_frames_sent = 0;
	filter->audio_frames_sent = 0;
	filter->last_wire_bytes = 0;
	filter->local_dropped_frames = 0;
	filter->tally_flags = 0;
	filter->current_bitrate_mbps = 0.0f;
	filter->current_rtt_ms = 0.0f;

	const char *codec_desc = "Raw";
	if (filter->is_audio_only)
		codec_desc = "Audio-Only";
	else if (filter->video_compression == OPENAVX_VC_SPEEDHQ)
		codec_desc = "SpeedHQ (SHQ3/SHQ2)";
	else if (filter->video_compression == OPENAVX_VC_H264)
		codec_desc = "H.264 (Hardware Auto)";
	else if (filter->video_compression == OPENAVX_VC_HEVC)
		codec_desc = "HEVC (Hardware Auto)";
	else if (filter->video_compression == OPENAVX_VC_AV1)
		codec_desc = "AV1 (Hardware Auto)";

	obs_log(LOG_INFO, "[filter] started ISO stream '%s' (codec=%s, transport=%d%s)", resolved_name, codec_desc,
		(int)cfg.transport, passphrase ? " [E2EE Encrypted]" : "");
	return true;
}

static void dispatch_mapped_video_frame(struct openavx_filter *filter, const uint8_t *data, uint32_t linesize,
					uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den)
{
	if (!filter || !filter->stream || !data || width == 0 || height == 0)
		return;

	oavx_video_frame_t vf;
	memset(&vf, 0, sizeof(vf));
	vf.width = width;
	vf.height = height;
	vf.scan_type = OAVX_SCAN_PROGRESSIVE;
	vf.framerate.num = fps_num;
	vf.framerate.den = fps_den;
	vf.aspect_ratio.num = width;
	vf.aspect_ratio.den = height ? height : 1;
	vf.pts_ns = (int64_t)os_gettime_ns();

	if (filter->video_compression == OPENAVX_VC_SPEEDHQ) {
		size_t uyvy_row_bytes = (size_t)width * 2;
		size_t uyvy_total = uyvy_row_bytes * height;
		size_t alpha_total = (size_t)width * height;

		if (!ensure_filter_staging_buffers(filter, uyvy_total, alpha_total)) {
			filter->local_dropped_frames++;
			return;
		}

		convert_rgba_to_uyva(data, linesize, width, height, filter->uyvy_buf, filter->alpha_buf);
		vf.fourcc = OAVX_FOURCC_VIDEO_TYPE_SHQ3;
		vf.planes[0] = filter->uyvy_buf;
		vf.strides[0] = uyvy_row_bytes;
		vf.planes[1] = filter->alpha_buf;
		vf.strides[1] = (size_t)width;
	} else {
		/* H.264, HEVC, and Uncompressed pass RGBA directly */
		vf.fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA;
		vf.planes[0] = (uint8_t *)data;
		vf.strides[0] = (size_t)linesize;
	}

	if (oavx_stream_send_video(filter->stream, &vf) == OAVX_SUCCESS)
		filter->video_frames_sent++;
	else
		filter->local_dropped_frames++;
}

static const char *openavx_filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_FILTER_NAME;
}

static const char *openavx_audio_filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_AUDIO_FILTER_NAME;
}

static void openavx_filter_destroy(void *data)
{
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter)
		return;

	pthread_mutex_lock(&filter->lock);
	filter->active = false;
	openavx_filter_stop_stream_locked(filter);

	if (filter->audio_callback_registered && filter->target_source) {
		obs_source_remove_audio_capture_callback(filter->target_source, on_filter_audio_capture, filter);
		filter->audio_callback_registered = false;
		filter->target_source = NULL;
	}
	pthread_mutex_unlock(&filter->lock);

	/* Release GPU resources inside graphics context */
	obs_enter_graphics();
	if (filter->texrender) {
		gs_texrender_destroy(filter->texrender);
		filter->texrender = NULL;
	}
	for (int i = 0; i < 2; ++i) {
		if (filter->stagesurf[i]) {
			gs_stagesurface_destroy(filter->stagesurf[i]);
			filter->stagesurf[i] = NULL;
		}
		filter->staged[i] = false;
	}
	obs_leave_graphics();

	if (filter->ctx) {
		oavx_context_destroy(filter->ctx);
		filter->ctx = NULL;
	}

	bfree(filter->stream_name);
	bfree(filter->groups);
	bfree(filter->passphrase);
	bfree(filter->bind_ip);
	bfree(filter->failover_target);
	bfree(filter->uyvy_buf);
	bfree(filter->alpha_buf);
	pthread_mutex_destroy(&filter->lock);
	bfree(filter);
}

static void openavx_filter_update(void *data, obs_data_t *settings)
{
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter || !settings)
		return;

	bool new_active = obs_data_get_bool(settings, "active");
	const char *new_name = obs_data_get_string(settings, "stream_name");
	const char *new_groups = obs_data_get_string(settings, "groups");
	const char *new_pass = obs_data_get_string(settings, "passphrase");
	const char *new_bind_ip = obs_data_get_string(settings, "bind_ip");
	const char *new_failover = obs_data_get_string(settings, "failover_target");
	int new_vc = (int)obs_data_get_int(settings, "video_compression");
	int new_q = clamp_speedhq_quality((int)obs_data_get_int(settings, "speedhq_quality"));
	int new_br = (int)obs_data_get_int(settings, "bitrate_kbps");
	int new_gop = (int)obs_data_get_int(settings, "keyframe_interval");
	int new_ac = (int)sanitize_audio_compression((int)obs_data_get_int(settings, "audio_compression"));
	int new_tr = (int)sanitize_transport((int)obs_data_get_int(settings, "transport"));

	if (new_br <= 0)
		new_br = 6000;
	if (new_gop <= 0)
		new_gop = 60;

	pthread_mutex_lock(&filter->lock);
	bool name_changed = (!filter->stream_name && new_name && new_name[0]) ||
			    (filter->stream_name && (!new_name || strcmp(filter->stream_name, new_name) != 0));
	bool pass_changed = (!filter->passphrase && new_pass && new_pass[0]) ||
			    (filter->passphrase && (!new_pass || strcmp(filter->passphrase, new_pass) != 0));
	bool bind_changed = (!filter->bind_ip && new_bind_ip && new_bind_ip[0]) ||
			    (filter->bind_ip && (!new_bind_ip || strcmp(filter->bind_ip, new_bind_ip) != 0));
	bool fo_changed =
		(!filter->failover_target && new_failover && new_failover[0]) ||
		(filter->failover_target && (!new_failover || strcmp(filter->failover_target, new_failover) != 0));
	bool comp_changed = (filter->video_compression != new_vc);
	bool active_changed = (filter->active != new_active);

	bfree(filter->stream_name);
	filter->stream_name = bstrdup(new_name ? new_name : "");
	bfree(filter->groups);
	filter->groups = bstrdup(new_groups ? new_groups : "");
	bfree(filter->passphrase);
	filter->passphrase = bstrdup(new_pass ? new_pass : "");
	bfree(filter->bind_ip);
	filter->bind_ip = bstrdup(new_bind_ip ? new_bind_ip : "");
	bfree(filter->failover_target);
	filter->failover_target = bstrdup(new_failover ? new_failover : "");
	filter->video_compression = new_vc;
	filter->speedhq_quality = new_q;
	filter->bitrate_kbps = (uint32_t)new_br;
	filter->keyframe_interval = (uint32_t)new_gop;
	filter->audio_compression = new_ac;
	filter->transport = new_tr;
	filter->active = new_active;

	if (!new_active) {
		openavx_filter_stop_stream_locked(filter);
	} else if (active_changed || name_changed || pass_changed || bind_changed || fo_changed || comp_changed ||
		   !filter->stream) {
		openavx_filter_start_stream_locked(filter);
	} else if (filter->stream) {
		oavx_speedhq_set_quality((uint8_t)new_q);
		oavx_stream_set_audio_compression(filter->stream, (oavx_audio_compression_mode_e)new_ac, 128);
		if (new_vc == OPENAVX_VC_H264)
			oavx_stream_set_video_compression(filter->stream, OAVX_FOURCC_VIDEO_TYPE_H264,
							  (uint32_t)new_br);
		else if (new_vc == OPENAVX_VC_HEVC)
			oavx_stream_set_video_compression(filter->stream, OAVX_FOURCC_VIDEO_TYPE_HEVC,
							  (uint32_t)new_br);
		else if (new_vc == OPENAVX_VC_AV1)
			oavx_stream_set_video_compression(filter->stream, OAVX_FOURCC_VIDEO_TYPE_AV01,
							  (uint32_t)new_br);
		else
			oavx_stream_set_video_compression(filter->stream, 0, 0);
	}
	pthread_mutex_unlock(&filter->lock);
}

static void *openavx_filter_create_internal(obs_data_t *settings, obs_source_t *source, bool is_audio_only)
{
	struct openavx_filter *filter = (struct openavx_filter *)bzalloc(sizeof(struct openavx_filter));
	if (!filter)
		return NULL;

	filter->source = source;
	filter->is_audio_only = is_audio_only;

	if (pthread_mutex_init(&filter->lock, NULL) != 0) {
		bfree(filter);
		return NULL;
	}

	filter->ctx = oavx_context_create();
	if (!filter->ctx) {
		obs_log(LOG_ERROR, "[filter] failed to create OpenAVX context");
		pthread_mutex_destroy(&filter->lock);
		bfree(filter);
		return NULL;
	}

	filter->speedhq_quality = 96;
	filter->bitrate_kbps = 6000;
	filter->keyframe_interval = 60;
	filter->sample_rate = 48000;
	filter->channels = 2;

	audio_t *obs_audio = obs_get_audio();
	if (obs_audio) {
		uint32_t sr = audio_output_get_sample_rate(obs_audio);
		size_t ch = audio_output_get_channels(obs_audio);
		if (sr > 0)
			filter->sample_rate = sr;
		if (ch > 0 && ch <= 8)
			filter->channels = ch;
	}

	openavx_filter_update(filter, settings);
	return filter;
}

static void *openavx_filter_create(obs_data_t *settings, obs_source_t *source)
{
	return openavx_filter_create_internal(settings, source, false);
}

static void *openavx_audio_filter_create(obs_data_t *settings, obs_source_t *source)
{
	return openavx_filter_create_internal(settings, source, true);
}

static void openavx_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "active", true);
	obs_data_set_default_string(settings, "stream_name", "");
	obs_data_set_default_string(settings, "groups", "");
	obs_data_set_default_string(settings, "passphrase", "");
	obs_data_set_default_string(settings, "bind_ip", "");
	obs_data_set_default_string(settings, "failover_target", "");
	obs_data_set_default_int(settings, "video_compression", OPENAVX_VC_SPEEDHQ);
	obs_data_set_default_int(settings, "speedhq_quality", 96);
	obs_data_set_default_int(settings, "bitrate_kbps", 6000);
	obs_data_set_default_int(settings, "keyframe_interval", 60);
	obs_data_set_default_int(settings, "audio_compression", OAVX_AUDIO_COMPRESSION_OFF);
	obs_data_set_default_int(settings, "transport", OAVX_TRANSPORT_TCP);
}

static void openavx_audio_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "active", true);
	obs_data_set_default_string(settings, "stream_name", "");
	obs_data_set_default_string(settings, "groups", "");
	obs_data_set_default_string(settings, "passphrase", "");
	obs_data_set_default_string(settings, "bind_ip", "");
	obs_data_set_default_string(settings, "failover_target", "");
	obs_data_set_default_int(settings, "audio_compression", OAVX_AUDIO_COMPRESSION_OFF);
	obs_data_set_default_int(settings, "transport", OAVX_TRANSPORT_TCP);
}

static bool filter_props_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	int vc = (int)obs_data_get_int(settings, "video_compression");
	bool is_shq = (vc == OPENAVX_VC_SPEEDHQ);
	bool is_hw = (vc == OPENAVX_VC_H264 || vc == OPENAVX_VC_HEVC || vc == OPENAVX_VC_AV1);

	obs_property_t *p_q = obs_properties_get(props, "speedhq_quality");
	obs_property_t *p_br = obs_properties_get(props, "bitrate_kbps");
	obs_property_t *p_gop = obs_properties_get(props, "keyframe_interval");

	if (p_q)
		obs_property_set_visible(p_q, is_shq);
	if (p_br)
		obs_property_set_visible(p_br, is_hw);
	if (p_gop)
		obs_property_set_visible(p_gop, is_hw);

	return true;
}

static void apply_filter_tooltips(obs_properties_t *props, bool has_video)
{
	if (!props)
		return;
	if (has_video) {
		obs_property_set_long_description(obs_properties_get(props, "video_compression"),
						  obs_module_text("OpenAVX.Output.Tip.VideoCompression"));
		obs_property_set_long_description(obs_properties_get(props, "speedhq_quality"),
						  obs_module_text("OpenAVX.Output.Tip.SpeedHQQuality"));
		obs_property_set_long_description(obs_properties_get(props, "bitrate_kbps"),
						  obs_module_text("OpenAVX.Output.Tip.Bitrate"));
		obs_property_set_long_description(obs_properties_get(props, "keyframe_interval"),
						  obs_module_text("OpenAVX.Output.Tip.KeyframeInterval"));
	}
	obs_property_set_long_description(obs_properties_get(props, "audio_compression"),
					  obs_module_text("OpenAVX.Output.Tip.AudioCompression"));
	obs_property_set_long_description(obs_properties_get(props, "groups"),
					  obs_module_text("OpenAVX.Output.Tip.Groups"));
	obs_property_set_long_description(obs_properties_get(props, "transport"),
					  obs_module_text("OpenAVX.Source.Tip.Transport"));
	obs_property_set_long_description(obs_properties_get(props, "bind_ip"),
					  obs_module_text("OpenAVX.Output.Tip.BindIP"));
	obs_property_set_long_description(obs_properties_get(props, "failover_target"),
					  obs_module_text("OpenAVX.Output.Tip.FailoverTarget"));
}

static obs_properties_t *openavx_filter_get_properties(void *data)
{
	struct openavx_filter *filter = (struct openavx_filter *)data;
	obs_properties_t *props = obs_properties_create();

	/* === Group 1: General === */
	obs_properties_t *gen_props = obs_properties_create();
	obs_properties_add_bool(gen_props, "active", TEXT_ACTIVE);
	obs_properties_add_text(gen_props, "stream_name", TEXT_STREAM_NAME, OBS_TEXT_DEFAULT);
	obs_properties_add_text(gen_props, "passphrase", TEXT_PASSPHRASE, OBS_TEXT_PASSWORD);

	char cur_status[320];
	if (filter) {
		pthread_mutex_lock(&filter->lock);
		snprintf(cur_status, sizeof(cur_status), "%s", filter->status_text[0] ? filter->status_text : "Ready");
		pthread_mutex_unlock(&filter->lock);
	} else {
		snprintf(cur_status, sizeof(cur_status), "OpenAVX ISO Output");
	}
	obs_property_t *p_stat = obs_properties_add_text(gen_props, "status_info", cur_status, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p_stat, OBS_TEXT_INFO_NORMAL);
	obs_properties_add_group(props, "grp_general", TEXT_GROUP_GENERAL, OBS_GROUP_NORMAL, gen_props);

	/* === Group 2: Encoding === */
	obs_properties_t *enc_props = obs_properties_create();
	obs_property_t *p_vc = obs_properties_add_list(enc_props, "video_compression", TEXT_VIDEO_COMPRESSION,
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_vc, TEXT_VC_UNCOMPRESSED, OPENAVX_VC_UNCOMPRESSED);
	obs_property_list_add_int(p_vc, TEXT_VC_SPEEDHQ, OPENAVX_VC_SPEEDHQ);
	obs_property_list_add_int(p_vc, TEXT_VC_H264, OPENAVX_VC_H264);
	obs_property_list_add_int(p_vc, TEXT_VC_HEVC, OPENAVX_VC_HEVC);
	obs_property_list_add_int(p_vc, TEXT_VC_AV1, OPENAVX_VC_AV1);
	obs_property_set_modified_callback(p_vc, filter_props_modified);

	obs_property_t *p_q =
		obs_properties_add_int_slider(enc_props, "speedhq_quality", TEXT_SPEEDHQ_QUALITY, 1, 100, 1);
	obs_property_t *p_br = obs_properties_add_int(enc_props, "bitrate_kbps", TEXT_BITRATE, 1000, 50000, 500);
	obs_property_t *p_gop =
		obs_properties_add_int(enc_props, "keyframe_interval", TEXT_KEYFRAME_INTERVAL, 15, 300, 5);

	if (filter) {
		pthread_mutex_lock(&filter->lock);
		int vc = filter->video_compression;
		pthread_mutex_unlock(&filter->lock);
		bool is_shq = (vc == OPENAVX_VC_SPEEDHQ);
		bool is_hw = (vc == OPENAVX_VC_H264 || vc == OPENAVX_VC_HEVC || vc == OPENAVX_VC_AV1);
		obs_property_set_visible(p_q, is_shq);
		obs_property_set_visible(p_br, is_hw);
		obs_property_set_visible(p_gop, is_hw);
	}

	obs_property_t *p_ac = obs_properties_add_list(enc_props, "audio_compression", TEXT_AUDIO_COMPRESSION,
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_ac, TEXT_AC_OFF, OAVX_AUDIO_COMPRESSION_OFF);
	obs_property_list_add_int(p_ac, TEXT_AC_ADAPTIVE, OAVX_AUDIO_COMPRESSION_ADAPTIVE);
	obs_property_list_add_int(p_ac, TEXT_AC_MANUAL, OAVX_AUDIO_COMPRESSION_MANUAL);
	obs_properties_add_group(props, "grp_encoding", TEXT_GROUP_ENCODING, OBS_GROUP_NORMAL, enc_props);

	/* === Group 3: Network === */
	obs_properties_t *net_props = obs_properties_create();
	obs_properties_add_text(net_props, "groups", TEXT_GROUPS, OBS_TEXT_DEFAULT);
	obs_property_t *p_tr = obs_properties_add_list(net_props, "transport", TEXT_TRANSPORT, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_TCP, OAVX_TRANSPORT_TCP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_RUDP, OAVX_TRANSPORT_RUDP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_UDP, OAVX_TRANSPORT_UDP);
	obs_properties_add_text(net_props, "bind_ip", TEXT_BIND_IP, OBS_TEXT_DEFAULT);
	obs_properties_add_text(net_props, "failover_target", TEXT_FAILOVER_TARGET, OBS_TEXT_DEFAULT);
	obs_properties_add_group(props, "grp_network", TEXT_GROUP_NETWORK, OBS_GROUP_NORMAL, net_props);
	apply_filter_tooltips(props, true);

	return props;
}

static obs_properties_t *openavx_audio_filter_get_properties(void *data)
{
	struct openavx_filter *filter = (struct openavx_filter *)data;
	obs_properties_t *props = obs_properties_create();

	/* === Group 1: General === */
	obs_properties_t *gen_props = obs_properties_create();
	obs_properties_add_bool(gen_props, "active", TEXT_ACTIVE);
	obs_properties_add_text(gen_props, "stream_name", TEXT_STREAM_NAME, OBS_TEXT_DEFAULT);
	obs_properties_add_text(gen_props, "passphrase", TEXT_PASSPHRASE, OBS_TEXT_PASSWORD);

	char cur_status[320];
	if (filter) {
		pthread_mutex_lock(&filter->lock);
		snprintf(cur_status, sizeof(cur_status), "%s", filter->status_text[0] ? filter->status_text : "Ready");
		pthread_mutex_unlock(&filter->lock);
	} else {
		snprintf(cur_status, sizeof(cur_status), "OpenAVX Audio ISO Output");
	}
	obs_property_t *p_stat = obs_properties_add_text(gen_props, "status_info", cur_status, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p_stat, OBS_TEXT_INFO_NORMAL);
	obs_properties_add_group(props, "grp_general", TEXT_GROUP_GENERAL, OBS_GROUP_NORMAL, gen_props);

	/* === Group 2: Encoding === */
	obs_properties_t *enc_props = obs_properties_create();
	obs_property_t *p_ac = obs_properties_add_list(enc_props, "audio_compression", TEXT_AUDIO_COMPRESSION,
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_ac, TEXT_AC_OFF, OAVX_AUDIO_COMPRESSION_OFF);
	obs_property_list_add_int(p_ac, TEXT_AC_ADAPTIVE, OAVX_AUDIO_COMPRESSION_ADAPTIVE);
	obs_property_list_add_int(p_ac, TEXT_AC_MANUAL, OAVX_AUDIO_COMPRESSION_MANUAL);
	obs_properties_add_group(props, "grp_encoding", TEXT_GROUP_ENCODING, OBS_GROUP_NORMAL, enc_props);

	/* === Group 3: Network === */
	obs_properties_t *net_props = obs_properties_create();
	obs_properties_add_text(net_props, "groups", TEXT_GROUPS, OBS_TEXT_DEFAULT);
	obs_property_t *p_tr = obs_properties_add_list(net_props, "transport", TEXT_TRANSPORT, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_TCP, OAVX_TRANSPORT_TCP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_RUDP, OAVX_TRANSPORT_RUDP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_UDP, OAVX_TRANSPORT_UDP);
	obs_properties_add_text(net_props, "bind_ip", TEXT_BIND_IP, OBS_TEXT_DEFAULT);
	obs_properties_add_text(net_props, "failover_target", TEXT_FAILOVER_TARGET, OBS_TEXT_DEFAULT);
	obs_properties_add_group(props, "grp_network", TEXT_GROUP_NETWORK, OBS_GROUP_NORMAL, net_props);
	apply_filter_tooltips(props, false);

	return props;
}

static void openavx_filter_add(void *data, obs_source_t *source)
{
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter)
		return;

	pthread_mutex_lock(&filter->lock);
	obs_source_t *target = obs_filter_get_target(filter->source);
	if (!target)
		target = source;

	if (target && !filter->audio_callback_registered) {
		filter->target_source = target;
		obs_source_add_audio_capture_callback(target, on_filter_audio_capture, filter);
		filter->audio_callback_registered = true;
	}

	/* Re-evaluate stream name with target name now that parent source is attached */
	if (filter->active)
		openavx_filter_start_stream_locked(filter);
	pthread_mutex_unlock(&filter->lock);
}

static void openavx_filter_remove(void *data, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter)
		return;

	pthread_mutex_lock(&filter->lock);
	openavx_filter_stop_stream_locked(filter);

	if (filter->audio_callback_registered && filter->target_source) {
		obs_source_remove_audio_capture_callback(filter->target_source, on_filter_audio_capture, filter);
		filter->audio_callback_registered = false;
		filter->target_source = NULL;
	}
	pthread_mutex_unlock(&filter->lock);
}

static void openavx_filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter || !filter->source)
		return;

	/* Always tell OBS to skip filter for direct drawing so parent source appearance is unmodified */
	obs_source_skip_video_filter(filter->source);

	if (filter->is_audio_only || !filter->active || !filter->stream)
		return;

	obs_source_t *target = obs_filter_get_target(filter->source);
	if (!target)
		return;

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (width == 0 || height == 0) {
		width = obs_source_get_width(target);
		height = obs_source_get_height(target);
	}
	if (width == 0 || height == 0)
		return;

	/* SpeedHQ requires width multiple of 16 for DCT macroblocks; others require even width */
	if (filter->video_compression == OPENAVX_VC_SPEEDHQ)
		width = (width + 15) & ~15U;
	else
		width = (width + 1) & ~1U;

	height = (height + 1) & ~1U;

	uint32_t fps_num = 60;
	uint32_t fps_den = 1;
	video_t *v = obs_get_video();
	if (v) {
		const struct video_output_info *voi = video_output_get_info(v);
		if (voi && voi->fps_num > 0 && voi->fps_den > 0) {
			fps_num = voi->fps_num;
			fps_den = voi->fps_den;
		}
	}

	pthread_mutex_lock(&filter->lock);
	if (!filter->active || !filter->stream) {
		pthread_mutex_unlock(&filter->lock);
		return;
	}

	/* Resize or allocate offscreen render target and staging surfaces */
	if (!filter->texrender || filter->cx != width || filter->cy != height) {
		if (filter->texrender) {
			gs_texrender_destroy(filter->texrender);
			filter->texrender = NULL;
		}
		for (int i = 0; i < 2; ++i) {
			if (filter->stagesurf[i]) {
				gs_stagesurface_destroy(filter->stagesurf[i]);
				filter->stagesurf[i] = NULL;
			}
			filter->staged[i] = false;
		}
		filter->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		filter->stagesurf[0] = gs_stagesurface_create(width, height, GS_RGBA);
		filter->stagesurf[1] = gs_stagesurface_create(width, height, GS_RGBA);
		filter->cx = width;
		filter->cy = height;
		filter->stage_idx = 0;
	}

	if (!filter->texrender || !filter->stagesurf[0] || !filter->stagesurf[1]) {
		pthread_mutex_unlock(&filter->lock);
		return;
	}

	/* Render parent source to offscreen texture */
	gs_texrender_reset(filter->texrender);
	if (gs_texrender_begin(filter->texrender, width, height)) {
		struct vec4 bg;
		vec4_zero(&bg);
		gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(target);
		gs_blend_state_pop();
		gs_texrender_end(filter->texrender);

		gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);
		if (tex) {
			int cur_idx = filter->stage_idx;
			gs_stage_texture(filter->stagesurf[cur_idx], tex);
			filter->staged[cur_idx] = true;

			/* Map from opposite surface from previous frame for zero-stall GPU pipeline */
			int prev_idx = cur_idx ^ 1;
			if (filter->staged[prev_idx]) {
				uint8_t *mapped_ptr = NULL;
				uint32_t mapped_linesize = 0;
				if (gs_stagesurface_map(filter->stagesurf[prev_idx], &mapped_ptr, &mapped_linesize)) {
					dispatch_mapped_video_frame(filter, mapped_ptr, mapped_linesize, width, height,
								    fps_num, fps_den);
					gs_stagesurface_unmap(filter->stagesurf[prev_idx]);
				}
			}
			filter->stage_idx = prev_idx;
		}
	}

	pthread_mutex_unlock(&filter->lock);
}

static struct obs_audio_data *openavx_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter || !audio || !audio->data[0] || audio->frames == 0)
		return audio;

	/* If audio capture callback is already handling audio from parent source, return audio unmodified */
	if (filter->audio_callback_registered)
		return audio;

	pthread_mutex_lock(&filter->lock);
	if (!filter->active || !filter->stream) {
		pthread_mutex_unlock(&filter->lock);
		return audio;
	}

	size_t ch = filter->channels ? filter->channels : 2;
	uint32_t rate = filter->sample_rate ? filter->sample_rate : 48000;

	oavx_audio_frame_t af;
	memset(&af, 0, sizeof(af));
	af.sample_rate = rate;
	af.channels = (uint16_t)ch;
	af.frames = audio->frames;
	af.format = OAVX_AUDIO_FMT_FLT_PLANAR;
	af.stride_bytes = (size_t)audio->frames * sizeof(float);
	af.pts_ns = (int64_t)audio->timestamp;

	for (size_t i = 0; i < ch && i < 8; ++i) {
		af.planes[i] = audio->data[i];
	}

	if (oavx_stream_send_audio(filter->stream, &af) == OAVX_SUCCESS)
		filter->audio_frames_sent++;

	pthread_mutex_unlock(&filter->lock);
	return audio;
}

static void openavx_filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct openavx_filter *filter = (struct openavx_filter *)data;
	if (!filter)
		return;

	pthread_mutex_lock(&filter->lock);
	if (!filter->active || !filter->stream) {
		snprintf(filter->status_text, sizeof(filter->status_text), "Stopped / Inactive");
		pthread_mutex_unlock(&filter->lock);
		return;
	}

	oavx_telemetry_t telem;
	if (oavx_stream_get_telemetry(filter->stream, &telem)) {
		filter->last_wire_bytes = telem.total_wire_bytes;
		filter->current_bitrate_mbps = telem.bitrate_mbps;
		filter->current_rtt_ms = telem.kernel_rtt_ms;
	}

	oavx_tally_t tally;
	if (oavx_stream_get_tally(filter->stream, &tally, 0)) {
		filter->tally_flags = tally.flags;
	} else {
		filter->tally_flags = 0;
	}

	const char *tally_str = "";
	if ((filter->tally_flags & OAVX_TALLY_PROGRAM) != 0 && (filter->tally_flags & OAVX_TALLY_RECORDING) != 0)
		tally_str = " [PGM+REC]";
	else if ((filter->tally_flags & OAVX_TALLY_PROGRAM) != 0)
		tally_str = " [PGM]";
	else if ((filter->tally_flags & OAVX_TALLY_PREVIEW) != 0 && (filter->tally_flags & OAVX_TALLY_RECORDING) != 0)
		tally_str = " [PVW+REC]";
	else if ((filter->tally_flags & OAVX_TALLY_PREVIEW) != 0)
		tally_str = " [PVW]";
	else if ((filter->tally_flags & OAVX_TALLY_RECORDING) != 0)
		tally_str = " [REC]";

	int clients = oavx_stream_get_connection_count(filter->stream, 0);

	if (filter->is_audio_only) {
		snprintf(filter->status_text, sizeof(filter->status_text),
			 "Active | Audio %uHz/%zuch | %.2f Mbps | RTT: %.1fms | %d clients | %" PRIu64 " frames%s",
			 filter->sample_rate, filter->channels, (double)filter->current_bitrate_mbps,
			 (double)filter->current_rtt_ms, clients, filter->audio_frames_sent, tally_str);
	} else {
		const char *c_str = (filter->video_compression == OPENAVX_VC_SPEEDHQ) ? "SpeedHQ"
				    : (filter->video_compression == OPENAVX_VC_H264)  ? "H.264"
				    : (filter->video_compression == OPENAVX_VC_HEVC)  ? "HEVC"
				    : (filter->video_compression == OPENAVX_VC_AV1)   ? "AV1"
										      : "Raw";
		snprintf(filter->status_text, sizeof(filter->status_text),
			 "Active | %ux%u | %s | %.1f Mbps | RTT: %.1fms | %d clients | %" PRIu64 " frames%s",
			 filter->cx, filter->cy, c_str, (double)filter->current_bitrate_mbps,
			 (double)filter->current_rtt_ms, clients, filter->video_frames_sent, tally_str);
	}
	pthread_mutex_unlock(&filter->lock);
}

struct obs_source_info openavx_filter_info = {
	.id = "openavx_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = openavx_filter_get_name,
	.create = openavx_filter_create,
	.destroy = openavx_filter_destroy,
	.update = openavx_filter_update,
	.get_defaults = openavx_filter_get_defaults,
	.get_properties = openavx_filter_get_properties,
	.video_render = openavx_filter_video_render,
	.video_tick = openavx_filter_video_tick,
	.filter_audio = openavx_filter_audio,
	.filter_add = openavx_filter_add,
	.filter_remove = openavx_filter_remove,
};

struct obs_source_info openavx_audio_filter_info = {
	.id = "openavx_audio_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = openavx_audio_filter_get_name,
	.create = openavx_audio_filter_create,
	.destroy = openavx_filter_destroy,
	.update = openavx_filter_update,
	.get_defaults = openavx_audio_filter_get_defaults,
	.get_properties = openavx_audio_filter_get_properties,
	.filter_audio = openavx_filter_audio,
	.video_tick = openavx_filter_video_tick,
	.filter_add = openavx_filter_add,
	.filter_remove = openavx_filter_remove,
};
