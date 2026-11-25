/*
OpenAVX for OBS Studio - Output Implementation
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "openavx-output.h"
#include "openavx-source.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <openavx.h>
#include <avx_hdr.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <util/threading.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TEXT_OUTPUT_NAME         obs_module_text("OpenAVX.Output.Name")
#define TEXT_OUTPUT_VIDEO_NAME   obs_module_text("OpenAVX.Output.VideoName")
#define TEXT_STREAM_NAME         obs_module_text("OpenAVX.Output.StreamName")
#define TEXT_GROUPS              obs_module_text("OpenAVX.Output.Groups")
#define TEXT_PASSPHRASE          obs_module_text("OpenAVX.Output.Passphrase")
#define TEXT_VIDEO_COMPRESSION   obs_module_text("OpenAVX.Output.VideoCompression")
#define TEXT_VC_UNCOMPRESSED     obs_module_text("OpenAVX.Output.VideoCompression.Uncompressed")
#define TEXT_VC_SPEEDHQ          obs_module_text("OpenAVX.Output.VideoCompression.SpeedHQ")
#define TEXT_VC_H264             obs_module_text("OpenAVX.Output.VideoCompression.H264")
#define TEXT_VC_HEVC             obs_module_text("OpenAVX.Output.VideoCompression.HEVC")
#define TEXT_VC_AV1              obs_module_text("OpenAVX.Output.VideoCompression.AV1")
#define TEXT_WEB_PREVIEW         obs_module_text("OpenAVX.Dialog.WebPreview")
#define TEXT_WEB_PREVIEW_PORT    obs_module_text("OpenAVX.Dialog.WebPreviewPort")
#define TEXT_RTSP_SERVER         obs_module_text("OpenAVX.Dialog.RtspServer")
#define TEXT_RTSP_PORT           obs_module_text("OpenAVX.Dialog.RtspPort")
#define TEXT_SPEEDHQ_QUALITY     obs_module_text("OpenAVX.Output.SpeedHQQuality")
#define TEXT_BITRATE             obs_module_text("OpenAVX.Output.Bitrate")
#define TEXT_KEYFRAME_INTERVAL   obs_module_text("OpenAVX.Output.KeyframeInterval")
#define TEXT_COLOR_FORMAT        obs_module_text("OpenAVX.Output.ColorFormat")
#define TEXT_CF_AUTO             obs_module_text("OpenAVX.Output.ColorFormat.Auto")
#define TEXT_CF_UYVY             obs_module_text("OpenAVX.Output.ColorFormat.UYVY")
#define TEXT_CF_BGRA             obs_module_text("OpenAVX.Output.ColorFormat.BGRA")
#define TEXT_CF_RGBA             obs_module_text("OpenAVX.Output.ColorFormat.RGBA")
#define TEXT_AUDIO_COMPRESSION   obs_module_text("OpenAVX.Output.AudioCompression")
#define TEXT_AC_OFF              obs_module_text("OpenAVX.Output.AudioCompression.Off")
#define TEXT_AC_ADAPTIVE         obs_module_text("OpenAVX.Output.AudioCompression.Adaptive")
#define TEXT_AC_MANUAL           obs_module_text("OpenAVX.Output.AudioCompression.Manual")
#define TEXT_TRANSPORT           obs_module_text("OpenAVX.Output.Transport")
#define TEXT_TRANSPORT_TCP       obs_module_text("OpenAVX.Output.Transport.TCP")
#define TEXT_TRANSPORT_RUDP      obs_module_text("OpenAVX.Output.Transport.RUDP")
#define TEXT_TRANSPORT_UDP       obs_module_text("OpenAVX.Output.Transport.UDP")
#define TEXT_BIND_IP             obs_module_text("OpenAVX.Output.BindIP")
#define TEXT_FAILOVER_TARGET     obs_module_text("OpenAVX.Output.FailoverTarget")

#define DEFAULT_STREAM_NAME "OpenAVX Program Output"

enum openavx_video_compression {
	OPENAVX_VC_UNCOMPRESSED = 0,
	OPENAVX_VC_SPEEDHQ = 1,
	OPENAVX_VC_H264 = 2,
	OPENAVX_VC_HEVC = 3,
	OPENAVX_VC_AV1 = 4,
};

struct openavx_output {
	obs_output_t *output;
	oavx_context_t *ctx;

	/* Guards lifecycle, configuration updates, and UI telemetry polling (never held by raw_video/raw_audio) */
	pthread_mutex_t lock;
	oavx_stream_t *stream;

	/* Configuration */
	char *stream_name;
	char *groups;
	char *passphrase;
	char *bind_ip;
	char *failover_target;
	int video_compression;
	int speedhq_quality;
	uint32_t bitrate_kbps;
	uint32_t keyframe_interval;
	int color_format;
	int audio_compression;
	int transport;
	bool web_preview_enabled;
	uint16_t web_preview_port;
	bool rtsp_enabled;
	uint16_t rtsp_port;

	/* Active session snapshot (immutable while active is true) */
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	enum video_format obs_video_format;
	enum video_format wire_video_format;
	enum video_colorspace colorspace;
	enum video_range_type range;
	bool active_speedhq;
	bool active_speedhq_alpha;
	bool is_hdr;
	char hdr_metadata_xml[256];
	size_t hdr_metadata_len;

	bool has_audio;
	uint32_t sample_rate;
	size_t channels;

	/* Video-thread-only staging buffers for SpeedHQ packing / RGB32->UYVA */
	uint8_t *uyvy_buf;
	size_t uyvy_cap;
	uint8_t *alpha_buf;
	size_t alpha_cap;

	/* Lockless media state & per-thread counters */
	volatile bool active;
	volatile long local_dropped_frames;
	uint64_t video_frames_sent;
	uint64_t audio_frames_sent;

	/* Cached telemetry & tally */
	uint64_t last_total_bytes;
	int last_dropped_frames;
	uint32_t tally_flags;
};

static bool obs_to_oavx_video_format(enum video_format format, oavx_fourcc_video_type_e *out_fourcc)
{
	switch (format) {
	case VIDEO_FORMAT_UYVY:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY;
		return true;
	case VIDEO_FORMAT_BGRA:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_BGRA;
		return true;
	case VIDEO_FORMAT_RGBA:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA;
		return true;
	case VIDEO_FORMAT_BGRX:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_BGRX;
		return true;
	case VIDEO_FORMAT_NV12:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_NV12;
		return true;
	case VIDEO_FORMAT_I420:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_I420;
		return true;
	case VIDEO_FORMAT_P216:
		*out_fourcc = OAVX_FOURCC_VIDEO_TYPE_P216;
		return true;
	default:
		return false;
	}
}

static bool video_format_has_alpha(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
	case VIDEO_FORMAT_AYUV:
	case VIDEO_FORMAT_YA2L:
		return true;
	default:
		return false;
	}
}

static bool video_format_is_high_depth(enum video_format format, enum video_colorspace cs)
{
	if (cs == VIDEO_CS_2100_PQ || cs == VIDEO_CS_2100_HLG)
		return true;

	switch (format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_YA2L:
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
	case VIDEO_FORMAT_V210:
	case VIDEO_FORMAT_R10L:
		return true;
	default:
		return false;
	}
}

static int clamp_speedhq_quality(int quality)
{
	if (quality < 20)
		return 20;
	if (quality > 98)
		return 98;
	return quality;
}

static oavx_audio_compression_mode_e sanitize_audio_compression(int mode)
{
	switch (mode) {
	case OAVX_AUDIO_COMPRESSION_MANUAL:
		return OAVX_AUDIO_COMPRESSION_MANUAL;
	case OAVX_AUDIO_COMPRESSION_ADAPTIVE:
		return OAVX_AUDIO_COMPRESSION_ADAPTIVE;
	default:
		return OAVX_AUDIO_COMPRESSION_OFF;
	}
}

static oavx_transport_e sanitize_transport(int transport)
{
	switch (transport) {
	case OAVX_TRANSPORT_RUDP:
		return OAVX_TRANSPORT_RUDP;
	case OAVX_TRANSPORT_UDP:
		return OAVX_TRANSPORT_UDP;
	default:
		return OAVX_TRANSPORT_TCP;
	}
}

static int add_clamped_dropped(uint64_t wire_dropped, long local_dropped)
{
	uint64_t total = wire_dropped + (local_dropped > 0 ? (uint64_t)local_dropped : 0ULL);
	if (total > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)total;
}

static bool ensure_video_staging_buffers(struct openavx_output *out, size_t uyvy_needed, size_t alpha_needed)
{
	if (uyvy_needed > out->uyvy_cap) {
		uint8_t *nb = (uint8_t *)brealloc(out->uyvy_buf, uyvy_needed);
		if (!nb)
			return false;
		out->uyvy_buf = nb;
		out->uyvy_cap = uyvy_needed;
	}

	if (alpha_needed > out->alpha_cap) {
		uint8_t *nb = (uint8_t *)brealloc(out->alpha_buf, alpha_needed);
		if (!nb)
			return false;
		out->alpha_buf = nb;
		out->alpha_cap = alpha_needed;
	}

	return true;
}

static inline uint8_t clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

static void convert_rgb32_to_uyva(const uint8_t *src, uint32_t src_stride, enum video_format src_fmt, uint32_t width,
				  uint32_t height, uint8_t *uyvy_dst, uint8_t *alpha_dst)
{
	size_t uyvy_row_bytes = (size_t)width * 2;
	bool use_rgba = (src_fmt == VIDEO_FORMAT_RGBA);

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *s = src + (size_t)y * src_stride;
		uint8_t *d = uyvy_dst + (size_t)y * uyvy_row_bytes;
		uint8_t *a = alpha_dst + (size_t)y * width;

		for (uint32_t x = 0; x + 1 < width; x += 2) {
			const uint8_t *p0 = s + (size_t)x * 4;
			const uint8_t *p1 = p0 + 4;

			int r0 = use_rgba ? p0[0] : p0[2];
			int g0 = p0[1];
			int b0 = use_rgba ? p0[2] : p0[0];

			int r1 = use_rgba ? p1[0] : p1[2];
			int g1 = p1[1];
			int b1 = use_rgba ? p1[2] : p1[0];

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

			a[x + 0] = p0[3];
			a[x + 1] = p1[3];
		}

		if ((width & 1U) != 0U) {
			uint32_t x = width - 1;
			const uint8_t *p0 = s + (size_t)x * 4;
			int r0 = use_rgba ? p0[0] : p0[2];
			int g0 = p0[1];
			int b0 = use_rgba ? p0[2] : p0[0];

			int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
			int u = ((-38 * r0 - 74 * g0 + 112 * b0 + 128) >> 8) + 128;
			int v = ((112 * r0 - 94 * g0 - 18 * b0 + 128) >> 8) + 128;

			size_t off = (size_t)x * 2;
			d[off + 0] = clamp_u8(u);
			d[off + 1] = clamp_u8(y0);
			d[off + 2] = clamp_u8(v);
			d[off + 3] = clamp_u8(y0);
			a[x] = p0[3];
		}
	}
}

static void negotiate_video_format(const struct video_output_info *voi, int video_compression, int color_format,
				   enum video_format *out_wire_fmt, bool *out_use_speedhq, bool *out_speedhq_alpha,
				   bool *out_is_hdr, struct video_scale_info *out_vsi, bool *out_need_conv)
{
	enum video_format obs_fmt = voi->format;
	bool has_alpha = video_format_has_alpha(obs_fmt);
	bool is_hdr = video_format_is_high_depth(obs_fmt, voi->colorspace);
	bool want_speedhq = (video_compression == OPENAVX_VC_SPEEDHQ);
	bool want_hw = (video_compression == OPENAVX_VC_H264 || video_compression == OPENAVX_VC_HEVC ||
			video_compression == OPENAVX_VC_AV1);

	*out_is_hdr = is_hdr;
	*out_use_speedhq = false;
	*out_speedhq_alpha = false;

	enum video_format target_fmt = VIDEO_FORMAT_NONE;

	if (want_hw) {
		if (is_hdr) {
			target_fmt = VIDEO_FORMAT_P216;
		} else if (has_alpha) {
			target_fmt = (obs_fmt == VIDEO_FORMAT_RGBA) ? VIDEO_FORMAT_RGBA : VIDEO_FORMAT_BGRA;
		} else {
			target_fmt = VIDEO_FORMAT_NV12;
		}
	} else if (want_speedhq) {
		if (is_hdr) {
			obs_log(LOG_WARNING,
				"[output] SpeedHQ supports 8-bit SDR only; transmitting HDR as uncompressed P216");
			target_fmt = VIDEO_FORMAT_P216;
		} else if ((voi->width % 16) != 0) {
			obs_log(LOG_WARNING,
				"[output] SpeedHQ requires width multiple of 16 (%ux%u); falling back to uncompressed",
				voi->width, voi->height);
			want_speedhq = false;
		} else {
			*out_use_speedhq = true;
			if (has_alpha) {
				*out_speedhq_alpha = true;
				target_fmt = (obs_fmt == VIDEO_FORMAT_RGBA) ? VIDEO_FORMAT_RGBA : VIDEO_FORMAT_BGRA;
			} else {
				target_fmt = VIDEO_FORMAT_UYVY;
			}
		}
	}

	if (!*out_use_speedhq && target_fmt == VIDEO_FORMAT_NONE) {
		if (is_hdr) {
			target_fmt = VIDEO_FORMAT_P216;
		} else {
			switch (color_format) {
			case OAVX_COLOR_FORMAT_UYVY_BGRA:
			case OAVX_COLOR_FORMAT_UYVY_RGBA:
				if (has_alpha)
					target_fmt = (color_format == OAVX_COLOR_FORMAT_UYVY_RGBA) ? VIDEO_FORMAT_RGBA
												   : VIDEO_FORMAT_BGRA;
				else
					target_fmt = VIDEO_FORMAT_UYVY;
				break;
			case OAVX_COLOR_FORMAT_BGRX_BGRA:
				target_fmt = has_alpha ? VIDEO_FORMAT_BGRA : VIDEO_FORMAT_BGRX;
				break;
			case OAVX_COLOR_FORMAT_RGBX_RGBA:
				target_fmt = VIDEO_FORMAT_RGBA;
				break;
			case OAVX_COLOR_FORMAT_BEST:
			case OAVX_COLOR_FORMAT_FASTEST:
			default: {
				oavx_fourcc_video_type_e dummy;
				if (obs_to_oavx_video_format(obs_fmt, &dummy))
					target_fmt = obs_fmt;
				else if (has_alpha)
					target_fmt = VIDEO_FORMAT_BGRA;
				else
					target_fmt = VIDEO_FORMAT_UYVY;
				break;
			}
			}
		}
	}

	*out_wire_fmt = target_fmt;
	*out_need_conv = (target_fmt != obs_fmt);

	memset(out_vsi, 0, sizeof(*out_vsi));
	out_vsi->format = target_fmt;
	out_vsi->width = voi->width;
	out_vsi->height = voi->height;
	if (target_fmt == VIDEO_FORMAT_P216) {
		out_vsi->colorspace = (voi->colorspace == VIDEO_CS_DEFAULT) ? VIDEO_CS_2100_PQ : voi->colorspace;
		out_vsi->range = (voi->range == VIDEO_RANGE_DEFAULT) ? VIDEO_RANGE_PARTIAL : voi->range;
	} else if (format_is_yuv(target_fmt)) {
		/* Keep BT.709/BT.601 YUV spaces; map sRGB canvas down to Rec.709 partial for UYVY wire parity */
		out_vsi->colorspace = (voi->colorspace == VIDEO_CS_601) ? VIDEO_CS_601 : VIDEO_CS_709;
		out_vsi->range = VIDEO_RANGE_PARTIAL;
	} else {
		out_vsi->colorspace = VIDEO_CS_SRGB;
		out_vsi->range = VIDEO_RANGE_FULL;
	}
}

static void populate_hdr_metadata(struct openavx_output *out, const struct video_output_info *voi)
{
	out->hdr_metadata_xml[0] = '\0';
	out->hdr_metadata_len = 0;

	if (!out->is_hdr)
		return;

	avx_color_info_t cinfo;
	memset(&cinfo, 0, sizeof(cinfo));
	if (voi->colorspace == VIDEO_CS_2100_PQ) {
		cinfo.transfer = AVX_COLOR_TRANSFER_BT2100_PQ;
		cinfo.matrix = AVX_COLOR_MATRIX_BT2020;
		cinfo.primaries = AVX_COLOR_PRIMARIES_BT2020;
	} else if (voi->colorspace == VIDEO_CS_2100_HLG) {
		cinfo.transfer = AVX_COLOR_TRANSFER_BT2100_HLG;
		cinfo.matrix = AVX_COLOR_MATRIX_BT2020;
		cinfo.primaries = AVX_COLOR_PRIMARIES_BT2020;
	} else {
		cinfo.transfer = AVX_COLOR_TRANSFER_SDR;
		cinfo.matrix = AVX_COLOR_MATRIX_BT709;
		cinfo.primaries = AVX_COLOR_PRIMARIES_BT709;
	}

	if (avx_hdr_format_color_info(&cinfo, out->hdr_metadata_xml, sizeof(out->hdr_metadata_xml)))
		out->hdr_metadata_len = strlen(out->hdr_metadata_xml);
}

static const char *openavx_output_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_OUTPUT_NAME;
}

static const char *openavx_video_output_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_OUTPUT_VIDEO_NAME;
}

static void openavx_output_destroy(void *data)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out)
		return;

	os_atomic_store_bool(&out->active, false);

	pthread_mutex_lock(&out->lock);
	if (out->stream) {
		oavx_stream_close(out->stream);
		out->stream = NULL;
	}
	pthread_mutex_unlock(&out->lock);

	if (out->ctx) {
		oavx_context_destroy(out->ctx);
		out->ctx = NULL;
	}

	bfree(out->stream_name);
	bfree(out->groups);
	bfree(out->passphrase);
	bfree(out->bind_ip);
	bfree(out->failover_target);
	bfree(out->uyvy_buf);
	bfree(out->alpha_buf);
	pthread_mutex_destroy(&out->lock);
	bfree(out);
}

static void openavx_output_update(void *data, obs_data_t *settings)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out || !settings)
		return;

	const char *new_name = obs_data_get_string(settings, "stream_name");
	const char *new_groups = obs_data_get_string(settings, "groups");
	const char *new_pass = obs_data_get_string(settings, "passphrase");
	const char *new_bind_ip = obs_data_get_string(settings, "bind_ip");
	const char *new_failover = obs_data_get_string(settings, "failover_target");
	int new_vc = (int)obs_data_get_int(settings, "video_compression");
	int new_q = clamp_speedhq_quality((int)obs_data_get_int(settings, "speedhq_quality"));
	int new_bitrate = (int)obs_data_get_int(settings, "bitrate_kbps");
	int new_gop = (int)obs_data_get_int(settings, "keyframe_interval");
	int new_cf = (int)obs_data_get_int(settings, "color_format");
	int new_ac = (int)sanitize_audio_compression((int)obs_data_get_int(settings, "audio_compression"));
	int new_tr = (int)sanitize_transport((int)obs_data_get_int(settings, "transport"));
	bool new_web = obs_data_get_bool(settings, "web_preview_enabled");
	int new_web_port = (int)obs_data_get_int(settings, "web_preview_port");
	bool new_rtsp = obs_data_get_bool(settings, "rtsp_enabled");
	int new_rtsp_port = (int)obs_data_get_int(settings, "rtsp_port");
	if (new_web_port <= 0 || new_web_port > 65535)
		new_web_port = 8080;
	if (new_rtsp_port <= 0 || new_rtsp_port > 65535)
		new_rtsp_port = 8554;

	if (new_bitrate <= 0)
		new_bitrate = 6000;
	if (new_gop <= 0)
		new_gop = 60;

	/* Migrate legacy settings where only "bandwidth" = OAVX_BANDWIDTH_LOWEST was saved */
	if (!obs_data_has_user_value(settings, "video_compression") && obs_data_has_user_value(settings, "bandwidth") &&
	    obs_data_get_int(settings, "bandwidth") == OAVX_BANDWIDTH_LOWEST) {
		new_vc = OPENAVX_VC_SPEEDHQ;
		if (!obs_data_has_user_value(settings, "audio_compression"))
			new_ac = OAVX_AUDIO_COMPRESSION_ADAPTIVE;
	}

	pthread_mutex_lock(&out->lock);
	bfree(out->stream_name);
	out->stream_name = bstrdup(new_name ? new_name : "");
	bfree(out->groups);
	out->groups = bstrdup(new_groups ? new_groups : "");
	bfree(out->passphrase);
	out->passphrase = bstrdup(new_pass ? new_pass : "");
	bfree(out->bind_ip);
	out->bind_ip = bstrdup(new_bind_ip ? new_bind_ip : "");
	bfree(out->failover_target);
	out->failover_target = bstrdup(new_failover ? new_failover : "");
	out->video_compression = new_vc;
	out->speedhq_quality = new_q;
	out->bitrate_kbps = (uint32_t)new_bitrate;
	out->keyframe_interval = (uint32_t)new_gop;
	out->color_format = new_cf;
	out->audio_compression = new_ac;
	out->transport = new_tr;
	out->web_preview_enabled = new_web;
	out->web_preview_port = (uint16_t)new_web_port;
	out->rtsp_enabled = new_rtsp;
	out->rtsp_port = (uint16_t)new_rtsp_port;

	if (out->stream) {
		oavx_speedhq_set_quality((uint8_t)new_q);
		oavx_stream_set_audio_compression(out->stream, (oavx_audio_compression_mode_e)new_ac, 128);
		if (new_vc == OPENAVX_VC_H264)
			oavx_stream_set_video_compression(out->stream, OAVX_FOURCC_VIDEO_TYPE_H264,
							  (uint32_t)new_bitrate);
		else if (new_vc == OPENAVX_VC_HEVC)
			oavx_stream_set_video_compression(out->stream, OAVX_FOURCC_VIDEO_TYPE_HEVC,
							  (uint32_t)new_bitrate);
		else if (new_vc == OPENAVX_VC_AV1)
			oavx_stream_set_video_compression(out->stream, OAVX_FOURCC_VIDEO_TYPE_AV01,
							  (uint32_t)new_bitrate);
		else
			oavx_stream_set_video_compression(out->stream, 0, 0);
	}
	pthread_mutex_unlock(&out->lock);
}

static void *openavx_output_create(obs_data_t *settings, obs_output_t *output)
{
	struct openavx_output *out = (struct openavx_output *)bzalloc(sizeof(struct openavx_output));
	out->output = output;

	if (pthread_mutex_init(&out->lock, NULL) != 0) {
		obs_log(LOG_ERROR, "[output] failed to initialize output mutex");
		bfree(out);
		return NULL;
	}

	out->ctx = oavx_context_create();
	if (!out->ctx) {
		obs_log(LOG_ERROR, "[output] failed to create OpenAVX context");
		pthread_mutex_destroy(&out->lock);
		bfree(out);
		return NULL;
	}

	out->speedhq_quality = 96;
	out->bitrate_kbps = 6000;
	out->keyframe_interval = 60;
	out->color_format = OAVX_COLOR_FORMAT_BEST;
	openavx_output_update(out, settings);
	return out;
}

static bool openavx_output_start(void *data)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out || !out->output)
		return false;

	if (!obs_output_can_begin_data_capture(out->output, 0)) {
		obs_log(LOG_WARNING, "[output] cannot begin data capture");
		return false;
	}

	video_t *video = obs_output_video(out->output);
	bool output_has_audio = (obs_output_get_flags(out->output) & OBS_OUTPUT_AUDIO) != 0;
	audio_t *audio = output_has_audio ? obs_output_audio(out->output) : NULL;

	const struct video_output_info *voi = video ? video_output_get_info(video) : NULL;
	if (!voi || voi->width == 0 || voi->height == 0) {
		obs_log(LOG_ERROR, "[output] no active OBS video context available");
		obs_output_set_last_error(out->output, "No active OBS video output context");
		return false;
	}

	const struct audio_output_info *aoi = audio ? audio_output_get_info(audio) : NULL;

	pthread_mutex_lock(&out->lock);
	if (out->stream) {
		oavx_stream_close(out->stream);
		out->stream = NULL;
	}

	int video_comp = out->video_compression;
	int speedhq_q = clamp_speedhq_quality(out->speedhq_quality);
	int color_fmt = out->color_format;
	oavx_audio_compression_mode_e audio_comp = sanitize_audio_compression(out->audio_compression);
	oavx_transport_e transport = sanitize_transport(out->transport);

	enum video_format wire_fmt = VIDEO_FORMAT_NONE;
	bool use_shq = false;
	bool shq_alpha = false;
	bool is_hdr = false;
	bool need_video_conv = false;
	struct video_scale_info vsi;
	memset(&vsi, 0, sizeof(vsi));

	negotiate_video_format(voi, video_comp, color_fmt, &wire_fmt, &use_shq, &shq_alpha, &is_hdr, &vsi,
			       &need_video_conv);

	out->width = voi->width;
	out->height = voi->height;
	out->fps_num = voi->fps_num ? voi->fps_num : 60;
	out->fps_den = voi->fps_den ? voi->fps_den : 1;
	out->obs_video_format = voi->format;
	out->wire_video_format = wire_fmt;
	out->colorspace = voi->colorspace;
	out->range = voi->range;
	out->active_speedhq = use_shq;
	out->active_speedhq_alpha = shq_alpha;
	out->is_hdr = is_hdr;
	populate_hdr_metadata(out, voi);

	struct audio_convert_info aci;
	memset(&aci, 0, sizeof(aci));
	if (audio) {
		uint32_t base_rate = (aoi && aoi->samples_per_sec > 0) ? aoi->samples_per_sec
								       : audio_output_get_sample_rate(audio);
		if (base_rate == 0)
			base_rate = 48000;

		/* OpenAVX Opus encoder frames are fixed to 960 samples (20ms @ 48kHz); resample when compressing */
		uint32_t wire_rate = (audio_comp != OAVX_AUDIO_COMPRESSION_OFF) ? 48000U : base_rate;
		enum speaker_layout spk = (aoi && aoi->speakers != SPEAKERS_UNKNOWN) ? aoi->speakers : SPEAKERS_STEREO;
		size_t ch = get_audio_channels(spk);
		if (ch == 0 || ch > 8) {
			spk = SPEAKERS_STEREO;
			ch = 2;
		}

		aci.samples_per_sec = wire_rate;
		aci.format = AUDIO_FORMAT_FLOAT_PLANAR;
		aci.speakers = spk;
		aci.allow_clipping = false;

		out->has_audio = true;
		out->sample_rate = wire_rate;
		out->channels = ch;
	} else {
		out->has_audio = false;
		out->sample_rate = 48000;
		out->channels = 2;
	}

	const char *stream_name = (out->stream_name && out->stream_name[0] != '\0') ? out->stream_name
										    : DEFAULT_STREAM_NAME;
	const char *groups = (out->groups && out->groups[0] != '\0') ? out->groups : NULL;
	const char *passphrase = (out->passphrase && out->passphrase[0] != '\0') ? out->passphrase : NULL;
	const char *bind_ip = (out->bind_ip && out->bind_ip[0] != '\0') ? out->bind_ip : NULL;

	oavx_stream_config_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.name = stream_name;
	cfg.groups = groups;
	cfg.direction = OAVX_DIR_TRANSMIT;
	cfg.bandwidth = OAVX_BANDWIDTH_HIGHEST;
	cfg.color_format = (oavx_color_format_e)color_fmt;
	cfg.transport = transport;
	cfg.clock_sync = false;
	cfg.bind_ip = bind_ip;
	cfg.passphrase = passphrase;

	if (video_comp == OPENAVX_VC_H264) {
		cfg.video_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
		cfg.video_bitrate_kbps = out->bitrate_kbps ? out->bitrate_kbps : 6000;
		cfg.keyframe_interval = out->keyframe_interval ? out->keyframe_interval : 60;
	} else if (video_comp == OPENAVX_VC_HEVC) {
		cfg.video_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_HEVC;
		cfg.video_bitrate_kbps = out->bitrate_kbps ? out->bitrate_kbps : 6000;
		cfg.keyframe_interval = out->keyframe_interval ? out->keyframe_interval : 60;
	} else if (video_comp == OPENAVX_VC_AV1) {
		cfg.video_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_AV01;
		cfg.video_bitrate_kbps = out->bitrate_kbps ? out->bitrate_kbps : 6000;
		cfg.keyframe_interval = out->keyframe_interval ? out->keyframe_interval : 60;
	}

	oavx_speedhq_set_quality((uint8_t)speedhq_q);

	out->stream = oavx_stream_open(out->ctx, &cfg);
	if (!out->stream) {
		pthread_mutex_unlock(&out->lock);
		obs_log(LOG_ERROR, "[output] failed to open OpenAVX transmission stream '%s'", stream_name);
		obs_output_set_last_error(out->output, "Failed to open OpenAVX transmission stream");
		return false;
	}

	if (audio_comp != OAVX_AUDIO_COMPRESSION_OFF)
		oavx_stream_set_audio_compression(out->stream, audio_comp, 128);

	if (out->failover_target && out->failover_target[0] != '\0') {
		char fo_name[256] = {0};
		char fo_addr[256] = {0};
		if (openavx_source_resolve_endpoint(out->failover_target, fo_name, sizeof(fo_name), fo_addr,
						    sizeof(fo_addr))) {
			oavx_endpoint_t fep;
			memset(&fep, 0, sizeof(fep));
			fep.name = fo_name[0] ? fo_name : out->failover_target;
			fep.address = fo_addr;
			oavx_stream_set_failover(out->stream, &fep);
			obs_log(LOG_INFO, "[output] advertising failover target '%s' (%s)", fep.name, fep.address);
		} else {
			obs_log(LOG_WARNING, "[output] could not resolve failover target '%s' to ip:port",
				out->failover_target);
		}
	}

	if (out->is_hdr && out->hdr_metadata_len > 0) {
		oavx_metadata_frame_t hdr_meta;
		memset(&hdr_meta, 0, sizeof(hdr_meta));
		hdr_meta.data = out->hdr_metadata_xml;
		hdr_meta.length = out->hdr_metadata_len;
		oavx_stream_add_connection_metadata(out->stream, &hdr_meta);
	}

	if (out->web_preview_enabled) {
		if (oavx_stream_enable_web_preview(out->stream, out->web_preview_port) == OAVX_SUCCESS) {
			char url_buf[256] = {0};
			oavx_stream_get_web_preview_url(out->stream, url_buf, sizeof(url_buf));
			obs_log(LOG_INFO, "[output] HTTP fMP4 web preview enabled at %s",
				url_buf[0] ? url_buf : "active");
		} else {
			obs_log(LOG_WARNING, "[output] failed to start HTTP fMP4 web preview on port %u",
				(unsigned)out->web_preview_port);
		}
	}

	if (out->rtsp_enabled) {
		if (oavx_stream_enable_rtsp_server(out->stream, out->rtsp_port, NULL) == OAVX_SUCCESS) {
			obs_log(LOG_INFO, "[output] RTSP 1.0 server enabled on port %u",
				(unsigned)oavx_stream_get_rtsp_port(out->stream));
		} else {
			obs_log(LOG_WARNING, "[output] failed to start RTSP 1.0 server on port %u",
				(unsigned)out->rtsp_port);
		}
	}

	out->video_frames_sent = 0;
	out->audio_frames_sent = 0;
	out->last_total_bytes = 0;
	out->last_dropped_frames = 0;
	out->tally_flags = 0;
	os_atomic_store_long(&out->local_dropped_frames, 0);
	os_atomic_store_bool(&out->active, true);

	char name_copy[256];
	snprintf(name_copy, sizeof(name_copy), "%.255s", stream_name);
	pthread_mutex_unlock(&out->lock);

	if (need_video_conv || obs_output_get_video_conversion(out->output) != NULL)
		obs_output_set_video_conversion(out->output, &vsi);
	if (audio)
		obs_output_set_audio_conversion(out->output, &aci);

	if (!obs_output_begin_data_capture(out->output, 0)) {
		os_atomic_store_bool(&out->active, false);
		pthread_mutex_lock(&out->lock);
		if (out->stream) {
			oavx_stream_close(out->stream);
			out->stream = NULL;
		}
		pthread_mutex_unlock(&out->lock);
		obs_log(LOG_ERROR, "[output] obs_output_begin_data_capture failed");
		obs_output_set_last_error(out->output, "Failed to start OBS media data capture");
		return false;
	}

	char audio_desc[48];
	if (out->has_audio)
		snprintf(audio_desc, sizeof(audio_desc), "FLTP %uHz/%zuch", out->sample_rate, out->channels);
	else
		snprintf(audio_desc, sizeof(audio_desc), "none");

	const char *codec_desc = "Raw";
	if (out->active_speedhq)
		codec_desc = out->active_speedhq_alpha ? "SpeedHQ SHQ3" : "SpeedHQ SHQ2";
	else if (video_comp == OPENAVX_VC_H264)
		codec_desc = "H.264 (Hardware Auto)";
	else if (video_comp == OPENAVX_VC_HEVC)
		codec_desc = "HEVC (Hardware Auto)";
	else if (video_comp == OPENAVX_VC_AV1)
		codec_desc = "AV1 (Hardware Auto)";

	obs_log(LOG_INFO, "[output] started '%s' (%ux%u @ %u/%u fps, wire=%s%s, codec=%s, audio=%s, transport=%d%s)",
		name_copy, out->width, out->height, out->fps_num, out->fps_den,
		get_video_format_name(out->wire_video_format), out->is_hdr ? " HDR" : "", codec_desc, audio_desc,
		(int)transport, passphrase ? " [E2EE Encrypted]" : "");
	return true;
}

static void snapshot_telemetry_locked(struct openavx_output *out, float *out_congestion)
{
	if (out_congestion)
		*out_congestion = 0.0f;

	if (!out->stream)
		return;

	oavx_telemetry_t telem;
	if (oavx_stream_get_telemetry(out->stream, &telem)) {
		out->last_total_bytes = telem.total_wire_bytes;
		uint64_t wire_dropped = telem.dropped_frames_total > 0 ? (uint64_t)telem.dropped_frames_total : 0ULL;
		out->last_dropped_frames =
			add_clamped_dropped(wire_dropped, os_atomic_load_long(&out->local_dropped_frames));

		if (out_congestion) {
			if (telem.congestion_level >= 2)
				*out_congestion = 1.0f;
			else if (telem.congestion_level == 1)
				*out_congestion = 0.5f;
		}
	}

	oavx_tally_t tally;
	if (oavx_stream_get_tally(out->stream, &tally, 0)) {
		out->tally_flags = tally.flags;
	} else {
		out->tally_flags = 0;
	}
}

uint32_t openavx_output_get_tally_flags(obs_output_t *output)
{
	if (!output)
		return 0;
	struct openavx_output *out = (struct openavx_output *)obs_output_get_type_data(output);
	if (!out)
		return 0;
	pthread_mutex_lock(&out->lock);
	snapshot_telemetry_locked(out, NULL);
	uint32_t flags = out->tally_flags;
	pthread_mutex_unlock(&out->lock);
	return flags;
}

int openavx_output_get_connection_count(obs_output_t *output)
{
	if (!output)
		return 0;
	struct openavx_output *out = (struct openavx_output *)obs_output_get_type_data(output);
	if (!out)
		return 0;
	pthread_mutex_lock(&out->lock);
	int count = out->stream ? oavx_stream_get_connection_count(out->stream, 0) : 0;
	pthread_mutex_unlock(&out->lock);
	return count;
}

bool openavx_output_get_web_preview_url(obs_output_t *output, char *out_url, size_t max_len)
{
	if (out_url && max_len > 0)
		out_url[0] = '\0';
	if (!output || !out_url || max_len == 0)
		return false;
	struct openavx_output *out = (struct openavx_output *)obs_output_get_type_data(output);
	if (!out)
		return false;
	pthread_mutex_lock(&out->lock);
	bool ok = false;
	if (out->stream && out->web_preview_enabled) {
		const char *u = oavx_stream_get_web_preview_url(out->stream, out_url, max_len);
		ok = (u != NULL && out_url[0] != '\0');
	}
	pthread_mutex_unlock(&out->lock);
	return ok;
}

uint16_t openavx_output_get_rtsp_port(obs_output_t *output)
{
	if (!output)
		return 0;
	struct openavx_output *out = (struct openavx_output *)obs_output_get_type_data(output);
	if (!out)
		return 0;
	pthread_mutex_lock(&out->lock);
	uint16_t port = (out->stream && out->rtsp_enabled) ? oavx_stream_get_rtsp_port(out->stream) : 0;
	pthread_mutex_unlock(&out->lock);
	return port;
}

static void openavx_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out)
		return;

	/* Always notify libobs that capture ended so stopping_event is signaled on force-stop re-entry */
	os_atomic_store_bool(&out->active, false);
	obs_output_end_data_capture(out->output);

	pthread_mutex_lock(&out->lock);
	if (!out->stream) {
		pthread_mutex_unlock(&out->lock);
		return;
	}
	snapshot_telemetry_locked(out, NULL);
	oavx_stream_close(out->stream);
	out->stream = NULL;
	uint64_t v_sent = out->video_frames_sent;
	uint64_t a_sent = out->audio_frames_sent;
	uint64_t bytes = out->last_total_bytes;
	int dropped = out->last_dropped_frames;
	pthread_mutex_unlock(&out->lock);

	obs_log(LOG_INFO, "[output] stopped (video=%" PRIu64 ", audio=%" PRIu64 ", wire_bytes=%" PRIu64 ", dropped=%d)",
		v_sent, a_sent, bytes, dropped);
}

static void openavx_output_raw_video(void *data, struct video_data *frame)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out || !frame || !os_atomic_load_bool(&out->active))
		return;

	oavx_stream_t *stream = out->stream;
	if (!stream || !frame->data[0])
		return;

	oavx_video_frame_t vf;
	memset(&vf, 0, sizeof(vf));
	vf.width = out->width;
	vf.height = out->height;
	vf.scan_type = OAVX_SCAN_PROGRESSIVE;
	vf.framerate.num = out->fps_num;
	vf.framerate.den = out->fps_den;
	vf.aspect_ratio.num = out->width;
	vf.aspect_ratio.den = out->height ? out->height : 1;
	vf.pts_ns = (int64_t)frame->timestamp;

	if (out->is_hdr && out->hdr_metadata_len > 0 && (out->video_frames_sent % 30) == 0) {
		vf.metadata = out->hdr_metadata_xml;
		vf.metadata_len = out->hdr_metadata_len;
	}

	if (out->active_speedhq) {
		if (out->width == 0 || out->height == 0 || out->width > 16384 || out->height > 16384) {
			os_atomic_inc_long(&out->local_dropped_frames);
			return;
		}
		size_t uyvy_row_bytes = (size_t)out->width * 2;
		size_t uyvy_total = uyvy_row_bytes * out->height;

		if (out->active_speedhq_alpha) {
			size_t alpha_total = (size_t)out->width * out->height;
			if (!ensure_video_staging_buffers(out, uyvy_total, alpha_total)) {
				os_atomic_inc_long(&out->local_dropped_frames);
				return;
			}
			convert_rgb32_to_uyva(frame->data[0], frame->linesize[0], out->wire_video_format, out->width,
					      out->height, out->uyvy_buf, out->alpha_buf);
			vf.fourcc = OAVX_FOURCC_VIDEO_TYPE_SHQ3;
			vf.planes[0] = out->uyvy_buf;
			vf.strides[0] = uyvy_row_bytes;
			vf.planes[1] = out->alpha_buf;
			vf.strides[1] = (size_t)out->width;
		} else {
			vf.fourcc = OAVX_FOURCC_VIDEO_TYPE_SHQ2;
			if (frame->linesize[0] == uyvy_row_bytes) {
				vf.planes[0] = frame->data[0];
				vf.strides[0] = frame->linesize[0];
			} else {
				if (!ensure_video_staging_buffers(out, uyvy_total, 0)) {
					os_atomic_inc_long(&out->local_dropped_frames);
					return;
				}
				size_t src_stride = frame->linesize[0];
				for (uint32_t y = 0; y < out->height; ++y) {
					memcpy(out->uyvy_buf + (size_t)y * uyvy_row_bytes,
					       frame->data[0] + (size_t)y * src_stride, uyvy_row_bytes);
				}
				vf.planes[0] = out->uyvy_buf;
				vf.strides[0] = uyvy_row_bytes;
			}
		}
	} else {
		oavx_fourcc_video_type_e fourcc;
		if (!obs_to_oavx_video_format(out->wire_video_format, &fourcc)) {
			os_atomic_inc_long(&out->local_dropped_frames);
			return;
		}
		vf.fourcc = fourcc;
		for (int i = 0; i < 4; ++i) {
			vf.planes[i] = frame->data[i];
			vf.strides[i] = frame->linesize[i];
		}
	}

	if (oavx_stream_send_video(stream, &vf) == OAVX_SUCCESS)
		out->video_frames_sent++;
	else
		os_atomic_inc_long(&out->local_dropped_frames);
}

static void openavx_output_raw_audio(void *data, struct audio_data *frames)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out || !frames || !out->has_audio || !os_atomic_load_bool(&out->active))
		return;

	oavx_stream_t *stream = out->stream;
	if (!stream || !frames->data[0] || frames->frames == 0)
		return;

	size_t ch = out->channels;
	if (ch == 0 || ch > 8)
		return;

	oavx_audio_frame_t af;
	memset(&af, 0, sizeof(af));
	af.sample_rate = out->sample_rate;
	af.channels = (uint16_t)ch;
	af.frames = frames->frames;
	af.format = OAVX_AUDIO_FMT_FLT_PLANAR;
	af.stride_bytes = (size_t)frames->frames * sizeof(float);
	af.pts_ns = (int64_t)frames->timestamp;

	for (size_t i = 0; i < ch; ++i) {
		if (!frames->data[i])
			return;
		af.planes[i] = frames->data[i];
	}

	if (oavx_stream_send_audio(stream, &af) == OAVX_SUCCESS)
		out->audio_frames_sent++;
}

static bool on_video_compression_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	if (!props || !settings)
		return true;

	int vc = (int)obs_data_get_int(settings, "video_compression");
	bool is_shq = (vc == OPENAVX_VC_SPEEDHQ);
	bool is_hw = (vc == OPENAVX_VC_H264 || vc == OPENAVX_VC_HEVC || vc == OPENAVX_VC_AV1);

	obs_property_t *p_quality = obs_properties_get(props, "speedhq_quality");
	if (p_quality)
		obs_property_set_visible(p_quality, is_shq);

	obs_property_t *p_bitrate = obs_properties_get(props, "bitrate_kbps");
	if (p_bitrate)
		obs_property_set_visible(p_bitrate, is_hw);

	obs_property_t *p_gop = obs_properties_get(props, "keyframe_interval");
	if (p_gop)
		obs_property_set_visible(p_gop, is_hw);

	return true;
}

static void openavx_output_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "stream_name", DEFAULT_STREAM_NAME);
	obs_data_set_default_string(settings, "groups", "");
	obs_data_set_default_string(settings, "passphrase", "");
	obs_data_set_default_string(settings, "bind_ip", "");
	obs_data_set_default_string(settings, "failover_target", "");
	obs_data_set_default_int(settings, "video_compression", OPENAVX_VC_UNCOMPRESSED);
	obs_data_set_default_int(settings, "speedhq_quality", 96);
	obs_data_set_default_int(settings, "bitrate_kbps", 6000);
	obs_data_set_default_int(settings, "keyframe_interval", 60);
	obs_data_set_default_int(settings, "color_format", OAVX_COLOR_FORMAT_BEST);
	obs_data_set_default_int(settings, "audio_compression", OAVX_AUDIO_COMPRESSION_OFF);
	obs_data_set_default_int(settings, "transport", OAVX_TRANSPORT_TCP);
	obs_data_set_default_bool(settings, "web_preview_enabled", false);
	obs_data_set_default_int(settings, "web_preview_port", 8080);
	obs_data_set_default_bool(settings, "rtsp_enabled", false);
	obs_data_set_default_int(settings, "rtsp_port", 8554);
}

static obs_properties_t *openavx_output_get_properties(void *data)
{
	struct openavx_output *out = (struct openavx_output *)data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "stream_name", TEXT_STREAM_NAME, OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "groups", TEXT_GROUPS, OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "passphrase", TEXT_PASSPHRASE, OBS_TEXT_PASSWORD);
	obs_properties_add_text(props, "bind_ip", TEXT_BIND_IP, OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "failover_target", TEXT_FAILOVER_TARGET, OBS_TEXT_DEFAULT);

	obs_property_t *p_vc = obs_properties_add_list(props, "video_compression", TEXT_VIDEO_COMPRESSION,
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_vc, TEXT_VC_UNCOMPRESSED, OPENAVX_VC_UNCOMPRESSED);
	obs_property_list_add_int(p_vc, TEXT_VC_SPEEDHQ, OPENAVX_VC_SPEEDHQ);
	obs_property_list_add_int(p_vc, TEXT_VC_H264, OPENAVX_VC_H264);
	obs_property_list_add_int(p_vc, TEXT_VC_HEVC, OPENAVX_VC_HEVC);
	obs_property_list_add_int(p_vc, TEXT_VC_AV1, OPENAVX_VC_AV1);
	obs_property_set_modified_callback(p_vc, on_video_compression_modified);

	obs_property_t *p_q = obs_properties_add_int_slider(props, "speedhq_quality", TEXT_SPEEDHQ_QUALITY, 20, 98, 2);
	obs_property_t *p_bitrate =
		obs_properties_add_int_slider(props, "bitrate_kbps", TEXT_BITRATE, 1000, 50000, 500);
	obs_property_t *p_gop =
		obs_properties_add_int_slider(props, "keyframe_interval", TEXT_KEYFRAME_INTERVAL, 15, 300, 1);

	if (out) {
		bool is_shq = (out->video_compression == OPENAVX_VC_SPEEDHQ);
		bool is_hw = (out->video_compression == OPENAVX_VC_H264 || out->video_compression == OPENAVX_VC_HEVC ||
			      out->video_compression == OPENAVX_VC_AV1);
		obs_property_set_visible(p_q, is_shq);
		obs_property_set_visible(p_bitrate, is_hw);
		obs_property_set_visible(p_gop, is_hw);
	}

	obs_property_t *p_cf = obs_properties_add_list(props, "color_format", TEXT_COLOR_FORMAT, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_cf, TEXT_CF_AUTO, OAVX_COLOR_FORMAT_BEST);
	obs_property_list_add_int(p_cf, TEXT_CF_UYVY, OAVX_COLOR_FORMAT_UYVY_BGRA);
	obs_property_list_add_int(p_cf, TEXT_CF_BGRA, OAVX_COLOR_FORMAT_BGRX_BGRA);
	obs_property_list_add_int(p_cf, TEXT_CF_RGBA, OAVX_COLOR_FORMAT_RGBX_RGBA);

	obs_property_t *p_ac = obs_properties_add_list(props, "audio_compression", TEXT_AUDIO_COMPRESSION,
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_ac, TEXT_AC_OFF, OAVX_AUDIO_COMPRESSION_OFF);
	obs_property_list_add_int(p_ac, TEXT_AC_ADAPTIVE, OAVX_AUDIO_COMPRESSION_ADAPTIVE);
	obs_property_list_add_int(p_ac, TEXT_AC_MANUAL, OAVX_AUDIO_COMPRESSION_MANUAL);

	obs_property_t *p_tr =
		obs_properties_add_list(props, "transport", TEXT_TRANSPORT, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_TCP, OAVX_TRANSPORT_TCP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_RUDP, OAVX_TRANSPORT_RUDP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_UDP, OAVX_TRANSPORT_UDP);

	obs_properties_add_bool(props, "web_preview_enabled", TEXT_WEB_PREVIEW);
	obs_properties_add_int(props, "web_preview_port", TEXT_WEB_PREVIEW_PORT, 1024, 65535, 1);
	obs_properties_add_bool(props, "rtsp_enabled", TEXT_RTSP_SERVER);
	obs_properties_add_int(props, "rtsp_port", TEXT_RTSP_PORT, 1024, 65535, 1);

	return props;
}

static uint64_t openavx_output_get_total_bytes(void *data)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out)
		return 0;

	pthread_mutex_lock(&out->lock);
	snapshot_telemetry_locked(out, NULL);
	uint64_t total = out->last_total_bytes;
	pthread_mutex_unlock(&out->lock);
	return total;
}

static int openavx_output_get_dropped_frames(void *data)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out)
		return 0;

	pthread_mutex_lock(&out->lock);
	snapshot_telemetry_locked(out, NULL);
	int dropped = out->last_dropped_frames;
	pthread_mutex_unlock(&out->lock);
	return dropped;
}

static float openavx_output_get_congestion(void *data)
{
	struct openavx_output *out = (struct openavx_output *)data;
	if (!out)
		return 0.0f;

	float congestion = 0.0f;
	pthread_mutex_lock(&out->lock);
	snapshot_telemetry_locked(out, &congestion);
	pthread_mutex_unlock(&out->lock);
	return congestion;
}

struct obs_output_info openavx_output_info = {
	.id = "openavx_output",
	.flags = OBS_OUTPUT_AV,
	.get_name = openavx_output_get_name,
	.create = openavx_output_create,
	.destroy = openavx_output_destroy,
	.start = openavx_output_start,
	.stop = openavx_output_stop,
	.raw_video = openavx_output_raw_video,
	.raw_audio = openavx_output_raw_audio,
	.update = openavx_output_update,
	.get_defaults = openavx_output_get_defaults,
	.get_properties = openavx_output_get_properties,
	.get_total_bytes = openavx_output_get_total_bytes,
	.get_dropped_frames = openavx_output_get_dropped_frames,
	.get_congestion = openavx_output_get_congestion,
};

struct obs_output_info openavx_video_output_info = {
	.id = "openavx_video_output",
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = openavx_video_output_get_name,
	.create = openavx_output_create,
	.destroy = openavx_output_destroy,
	.start = openavx_output_start,
	.stop = openavx_output_stop,
	.raw_video = openavx_output_raw_video,
	.update = openavx_output_update,
	.get_defaults = openavx_output_get_defaults,
	.get_properties = openavx_output_get_properties,
	.get_total_bytes = openavx_output_get_total_bytes,
	.get_dropped_frames = openavx_output_get_dropped_frames,
	.get_congestion = openavx_output_get_congestion,
};
