/*
OpenAVX for OBS Studio - Source Implementation
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "openavx-source.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <openavx.h>
#include <avx_hdr.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <util/threading.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

#define TEXT_SOURCE_NAME        obs_module_text("OpenAVX.Source.Name")
#define TEXT_STREAM_NAME        obs_module_text("OpenAVX.Source.StreamName")
#define TEXT_REFRESH_STREAMS    obs_module_text("OpenAVX.Source.RefreshStreams")
#define TEXT_PASSPHRASE         obs_module_text("OpenAVX.Source.Passphrase")
#define TEXT_BANDWIDTH          obs_module_text("OpenAVX.Source.Bandwidth")
#define TEXT_BW_HIGHEST         obs_module_text("OpenAVX.Source.Bandwidth.Highest")
#define TEXT_BW_LOWEST          obs_module_text("OpenAVX.Source.Bandwidth.Lowest")
#define TEXT_BW_AUDIO_ONLY      obs_module_text("OpenAVX.Source.Bandwidth.AudioOnly")
#define TEXT_COLOR_FORMAT       obs_module_text("OpenAVX.Source.ColorFormat")
#define TEXT_CF_AUTO            obs_module_text("OpenAVX.Source.ColorFormat.Auto")
#define TEXT_CF_BGRA            obs_module_text("OpenAVX.Source.ColorFormat.BGRA")
#define TEXT_CF_RGBA            obs_module_text("OpenAVX.Source.ColorFormat.RGBA")
#define TEXT_CF_NATIVE          obs_module_text("OpenAVX.Source.ColorFormat.Native")
#define TEXT_COLOR_RANGE        obs_module_text("OpenAVX.Source.ColorRange")
#define TEXT_CR_AUTO            obs_module_text("OpenAVX.Source.ColorRange.Auto")
#define TEXT_CR_PARTIAL         obs_module_text("OpenAVX.Source.ColorRange.Partial")
#define TEXT_CR_FULL            obs_module_text("OpenAVX.Source.ColorRange.Full")
#define TEXT_COLOR_SPACE        obs_module_text("OpenAVX.Source.ColorSpace")
#define TEXT_CS_AUTO            obs_module_text("OpenAVX.Source.ColorSpace.Auto")
#define TEXT_CS_BT709           obs_module_text("OpenAVX.Source.ColorSpace.BT709")
#define TEXT_CS_BT601           obs_module_text("OpenAVX.Source.ColorSpace.BT601")
#define TEXT_CS_BT2100PQ        obs_module_text("OpenAVX.Source.ColorSpace.BT2100PQ")
#define TEXT_CS_BT2100HLG       obs_module_text("OpenAVX.Source.ColorSpace.BT2100HLG")
#define TEXT_BUFFER_MODE        obs_module_text("OpenAVX.Source.BufferMode")
#define TEXT_BM_LOW_LATENCY     obs_module_text("OpenAVX.Source.BufferMode.LowLatency")
#define TEXT_BM_FRAMESYNC       obs_module_text("OpenAVX.Source.BufferMode.FrameSync")
#define TEXT_BEHAVIOR           obs_module_text("OpenAVX.Source.Behavior")
#define TEXT_BH_KEEP_RUNNING    obs_module_text("OpenAVX.Source.Behavior.KeepRunning")
#define TEXT_BH_STOP_HIDDEN     obs_module_text("OpenAVX.Source.Behavior.StopWhenHidden")
#define TEXT_CLEAR_DISCONNECT   obs_module_text("OpenAVX.Source.ClearOnDisconnect")
#define TEXT_TRANSPORT          obs_module_text("OpenAVX.Source.Transport")
#define TEXT_TRANSPORT_TCP      obs_module_text("OpenAVX.Source.Transport.TCP")
#define TEXT_TRANSPORT_RUDP     obs_module_text("OpenAVX.Source.Transport.RUDP")
#define TEXT_TRANSPORT_UDP      obs_module_text("OpenAVX.Source.Transport.UDP")
#define TEXT_STATUS             obs_module_text("OpenAVX.Source.Status")
#define TEXT_CLOUD_TALLY        obs_module_text("OpenAVX.Source.CloudTally")
#define TEXT_CLOUD_TALLY_URL    obs_module_text("OpenAVX.Source.CloudTallyUrl")
#define TEXT_CLOUD_ROOM         obs_module_text("OpenAVX.Source.CloudRoomSlug")
#define TEXT_CLOUD_KEY          obs_module_text("OpenAVX.Source.CloudStreamKey")
#define TEXT_CLOUD_SLOT         obs_module_text("OpenAVX.Source.CloudSlotIndex")
#define TEXT_FAILOVER_SOURCE    obs_module_text("OpenAVX.Source.FailoverSource")
#define TEXT_FAILOVER_TIMEOUT   obs_module_text("OpenAVX.Source.FailoverTimeout")
#define TEXT_AUTO_REVERT        obs_module_text("OpenAVX.Source.AutoRevert")
#define TEXT_AUTO_REVERT_HOLD   obs_module_text("OpenAVX.Source.AutoRevertHold")
#define TEXT_DISCOVERY_GROUPS   obs_module_text("OpenAVX.Source.DiscoveryGroups")
#define TEXT_PTZ_PAN            obs_module_text("OpenAVX.Source.PtzPan")
#define TEXT_PTZ_TILT           obs_module_text("OpenAVX.Source.PtzTilt")
#define TEXT_PTZ_ZOOM           obs_module_text("OpenAVX.Source.PtzZoom")
#define TEXT_PTZ_FOCUS          obs_module_text("OpenAVX.Source.PtzFocus")
#define TEXT_PTZ_SEND           obs_module_text("OpenAVX.Source.PtzSend")
#define TEXT_RECORD_FILENAME    obs_module_text("OpenAVX.Source.RecordFilename")
#define TEXT_GROUP_GENERAL      obs_module_text("OpenAVX.Source.Group.General")
#define TEXT_GROUP_VIDEO_NET    obs_module_text("OpenAVX.Source.Group.VideoNetwork")
#define TEXT_GROUP_FAILOVER     obs_module_text("OpenAVX.Source.Group.Failover")
#define TEXT_GROUP_PTZ_REC      obs_module_text("OpenAVX.Source.Group.PtzRecording")
#define TEXT_UI_SECTION         obs_module_text("OpenAVX.Source.Section")
#define TEXT_UI_SEC_VIDEO_NET   obs_module_text("OpenAVX.Source.Section.VideoNetwork")
#define TEXT_UI_SEC_PTZ_REC     obs_module_text("OpenAVX.Source.Section.PtzRecording")
#define TEXT_UI_SEC_ALL         obs_module_text("OpenAVX.Source.Section.All")
#define TEXT_RECORD_START       obs_module_text("OpenAVX.Source.RecordStart")
#define TEXT_RECORD_STOP        obs_module_text("OpenAVX.Source.RecordStop")

#define MAX_CACHED_DISCOVERY 64

enum openavx_color_range {
	OPENAVX_COLOR_RANGE_AUTO = 0,
	OPENAVX_COLOR_RANGE_PARTIAL = 1,
	OPENAVX_COLOR_RANGE_FULL = 2,
};

enum openavx_color_space {
	OPENAVX_COLOR_SPACE_AUTO = 0,
	OPENAVX_COLOR_SPACE_BT709 = 1,
	OPENAVX_COLOR_SPACE_BT601 = 2,
	OPENAVX_COLOR_SPACE_BT2100_PQ = 3,
	OPENAVX_COLOR_SPACE_BT2100_HLG = 4,
};

enum openavx_buffer_mode {
	OPENAVX_BUFFER_LOW_LATENCY = 0,
	OPENAVX_BUFFER_FRAMESYNC = 1,
};

enum openavx_inactive_behavior {
	OPENAVX_BEHAVIOR_KEEP_RUNNING = 0,
	OPENAVX_BEHAVIOR_STOP_HIDDEN = 1,
};

struct discovered_entry {
	char name[256];
	char address[256];
};

/* Shared background discovery cache (keeps UI thread non-blocking) */
static pthread_mutex_t g_disc_lock;
static bool g_disc_lock_init = false;
static pthread_t g_disc_thread;
static bool g_disc_thread_started = false;
static volatile bool g_disc_running = false;
static os_event_t *g_disc_wake = NULL;
static oavx_context_t *g_disc_ctx = NULL;
static oavx_discovery_t *g_disc_handle = NULL;
static struct discovered_entry g_disc_cache[MAX_CACHED_DISCOVERY];
static size_t g_disc_count = 0;
static char g_disc_groups[256] = {0};
static bool g_disc_groups_dirty = false;

struct openavx_source {
	obs_source_t *source;
	oavx_context_t *ctx;

	/* Configuration (guarded by cfg_lock when read by worker on start) */
	pthread_mutex_t cfg_lock;
	char *source_name;
	char *passphrase;
	int bandwidth;
	int color_format;
	int color_range;
	int color_space;
	int buffer_mode;
	int behavior;
	int transport;
	bool clear_on_disconnect;
	char *failover_source;
	int failover_timeout_ms;
	bool auto_revert;
	int auto_revert_hold_ms;
	char *discovery_groups;

	/* PTZ & Remote Recording state (queued safely for recv worker) */
	double ptz_pan;
	double ptz_tilt;
	double ptz_zoom;
	double ptz_focus;
	bool ptz_pending;
	char *record_filename;
	int record_cmd_pending; /* 0 = none, 1 = start, 2 = stop */

	/* Worker lifecycle */
	pthread_t recv_thread;
	bool thread_started;
	volatile bool running;
	volatile long desired_tally;
	volatile bool obs_hidden;
	uint64_t last_tally_eval_ns;

	/* Reusable pixel scratch buffers for fallback conversions */
	uint8_t *scratch_buf;
	size_t scratch_cap;

	/* Metrics & timestamp anchor */
	volatile long width;
	volatile long height;
	uint64_t video_frames;
	uint64_t audio_frames;
	bool has_pts_offset;
	int64_t pts_offset_ns;
	uint64_t last_pts_anchor_ns;

	/* Live Telemetry string for status property */
	pthread_mutex_t telem_lock;
	char telem_status[256];

	/* Cloud Tally integration */
	bool cloud_tally_enabled;
	char *cloud_tally_url;
	char *cloud_room;
	char *cloud_key;
	int cloud_slot;

	pthread_t cloud_thread;
	bool cloud_thread_started;
	volatile bool cloud_running;
	os_event_t *cloud_wake;
	volatile long cloud_target_tally;
	long cloud_last_sent_tally;
};

static void *discovery_watcher_worker(void *param)
{
	UNUSED_PARAMETER(param);

	while (os_atomic_load_bool(&g_disc_running)) {
		bool need_recreate = false;
		char groups_copy[256] = {0};
		pthread_mutex_lock(&g_disc_lock);
		if (g_disc_groups_dirty) {
			need_recreate = true;
			snprintf(groups_copy, sizeof(groups_copy), "%s", g_disc_groups);
			g_disc_groups_dirty = false;
			g_disc_count = 0;
		}
		pthread_mutex_unlock(&g_disc_lock);

		if (need_recreate && g_disc_ctx) {
			if (g_disc_handle) {
				oavx_discovery_destroy(g_disc_handle);
				g_disc_handle = NULL;
			}
			g_disc_handle = oavx_discovery_create(g_disc_ctx, groups_copy[0] ? groups_copy : NULL);
			if (g_disc_handle)
				oavx_discovery_wait(g_disc_handle, 50);
		}

		if (g_disc_handle) {
			oavx_endpoint_t sources[MAX_CACHED_DISCOVERY];
			size_t n = oavx_discovery_get_sources(g_disc_handle, sources, MAX_CACHED_DISCOVERY);

			pthread_mutex_lock(&g_disc_lock);
			g_disc_count = 0;
			for (size_t i = 0; i < n && g_disc_count < MAX_CACHED_DISCOVERY; ++i) {
				if (!sources[i].name || !sources[i].name[0])
					continue;
				snprintf(g_disc_cache[g_disc_count].name, sizeof(g_disc_cache[g_disc_count].name), "%s",
					 sources[i].name);
				snprintf(g_disc_cache[g_disc_count].address, sizeof(g_disc_cache[g_disc_count].address),
					 "%s", sources[i].address ? sources[i].address : "");
				g_disc_count++;
			}
			pthread_mutex_unlock(&g_disc_lock);
		}

		if (g_disc_wake) {
			os_event_timedwait(g_disc_wake, 1000);
		} else {
			os_sleep_ms(1000);
		}
	}

	return NULL;
}

/* Global active source registry for instant frontend scene/tally notifications */
static pthread_mutex_t g_sources_lock = PTHREAD_MUTEX_INITIALIZER;
static struct openavx_source *g_active_sources[64];
static size_t g_num_active_sources = 0;

static void update_tally_from_obs(struct openavx_source *src);

static void register_active_source(struct openavx_source *src)
{
	if (!src)
		return;
	pthread_mutex_lock(&g_sources_lock);
	if (g_num_active_sources < 64) {
		g_active_sources[g_num_active_sources++] = src;
	}
	pthread_mutex_unlock(&g_sources_lock);
}

static void unregister_active_source(struct openavx_source *src)
{
	if (!src)
		return;
	pthread_mutex_lock(&g_sources_lock);
	for (size_t i = 0; i < g_num_active_sources; ++i) {
		if (g_active_sources[i] == src) {
			g_active_sources[i] = g_active_sources[--g_num_active_sources];
			break;
		}
	}
	pthread_mutex_unlock(&g_sources_lock);
}

static void notify_all_sources_tally(void)
{
	pthread_mutex_lock(&g_sources_lock);
	for (size_t i = 0; i < g_num_active_sources; ++i) {
		update_tally_from_obs(g_active_sources[i]);
	}
	pthread_mutex_unlock(&g_sources_lock);
}

static void on_frontend_event_cb(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		notify_all_sources_tally();
		break;
	default:
		break;
	}
}

void openavx_source_global_init(void)
{
	if (g_disc_lock_init)
		return;

	if (pthread_mutex_init(&g_disc_lock, NULL) != 0)
		return;
	g_disc_lock_init = true;

	os_event_init(&g_disc_wake, OS_EVENT_TYPE_AUTO);

	g_disc_ctx = oavx_context_create();
	if (g_disc_ctx) {
		g_disc_handle = oavx_discovery_create(g_disc_ctx, NULL);
		/* Trigger one brief initial drain so first Properties open has LAN entries */
		if (g_disc_handle)
			oavx_discovery_wait(g_disc_handle, 50);
	}

	os_atomic_store_bool(&g_disc_running, true);
	if (pthread_create(&g_disc_thread, NULL, discovery_watcher_worker, NULL) == 0) {
		g_disc_thread_started = true;
	} else {
		os_atomic_store_bool(&g_disc_running, false);
		obs_log(LOG_WARNING, "[source] failed to start background discovery thread");
	}

	if (obs_frontend_add_event_callback) {
		obs_frontend_add_event_callback(on_frontend_event_cb, NULL);
	}
}

void openavx_source_global_shutdown(void)
{
	if (!g_disc_lock_init)
		return;

	if (obs_frontend_remove_event_callback) {
		obs_frontend_remove_event_callback(on_frontend_event_cb, NULL);
	}

	os_atomic_store_bool(&g_disc_running, false);
	if (g_disc_wake)
		os_event_signal(g_disc_wake);

	if (g_disc_thread_started) {
		pthread_join(g_disc_thread, NULL);
		g_disc_thread_started = false;
	}

	if (g_disc_handle) {
		oavx_discovery_destroy(g_disc_handle);
		g_disc_handle = NULL;
	}
	if (g_disc_ctx) {
		oavx_context_destroy(g_disc_ctx);
		g_disc_ctx = NULL;
	}
	if (g_disc_wake) {
		os_event_destroy(g_disc_wake);
		g_disc_wake = NULL;
	}

	pthread_mutex_lock(&g_disc_lock);
	g_disc_count = 0;
	pthread_mutex_unlock(&g_disc_lock);

	pthread_mutex_destroy(&g_disc_lock);
	g_disc_lock_init = false;
}

static bool lookup_discovered_endpoint(const char *stream_name, char *out_ep, size_t ep_sz)
{
	if (!stream_name || !stream_name[0] || !out_ep || ep_sz == 0)
		return false;
	out_ep[0] = '\0';

	if (!g_disc_lock_init)
		return false;

	bool found = false;
	pthread_mutex_lock(&g_disc_lock);
	for (size_t i = 0; i < g_disc_count; ++i) {
		if (strcmp(g_disc_cache[i].name, stream_name) == 0 && g_disc_cache[i].address[0] != '\0') {
			snprintf(out_ep, ep_sz, "%s", g_disc_cache[i].address);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_disc_lock);
	return found;
}

static bool is_host_or_endpoint_token(const char *s)
{
	if (!s || !s[0])
		return false;
	if (s[0] == '[')
		return true;
	if (strchr(s, ':') != NULL)
		return true;
	if (strstr(s, "127.0.0.1") != NULL || strstr(s, "localhost") != NULL || strstr(s, ".local") != NULL)
		return true;
	return false;
}

static void parse_source_endpoint(const char *input, char *out_name, size_t name_sz, char *out_endpoint, size_t ep_sz)
{
	out_name[0] = '\0';
	out_endpoint[0] = '\0';

	if (!input || input[0] == '\0')
		return;

	/* Check for format: "Name (Endpoint)" */
	const char *open_paren = strrchr(input, '(');
	const char *close_paren = strrchr(input, ')');
	if (open_paren && close_paren && close_paren > open_paren + 1) {
		size_t inner_len = (size_t)(close_paren - (open_paren + 1));
		char inner[256] = {0};
		if (inner_len < sizeof(inner)) {
			memcpy(inner, open_paren + 1, inner_len);
			inner[inner_len] = '\0';
		}

		size_t prefix_len = (size_t)(open_paren - input);
		char prefix[256] = {0};
		if (prefix_len < sizeof(prefix)) {
			memcpy(prefix, input, prefix_len);
			prefix[prefix_len] = '\0';
		}
		while (prefix_len > 0 && (prefix[prefix_len - 1] == ' ' || prefix[prefix_len - 1] == '\t')) {
			prefix[--prefix_len] = '\0';
		}

		if (is_host_or_endpoint_token(inner)) {
			snprintf(out_endpoint, ep_sz, "%s", inner);
			snprintf(out_name, name_sz, "%s", prefix[0] ? prefix : "OpenAVX Stream");
			return;
		}
	}

	/* Bare IP, [IPv6]:port, or IP:port */
	if (is_host_or_endpoint_token(input)) {
		snprintf(out_endpoint, ep_sz, "%s", input);
		snprintf(out_name, name_sz, "OpenAVX Stream");
		return;
	}

	/* Pure stream name to resolve dynamically via discovery */
	snprintf(out_name, name_sz, "%s", input);
	out_endpoint[0] = '\0';
}

bool openavx_source_resolve_endpoint(const char *input, char *out_name, size_t name_sz, char *out_addr, size_t addr_sz)
{
	if (!out_name || name_sz == 0 || !out_addr || addr_sz == 0)
		return false;
	out_name[0] = '\0';
	out_addr[0] = '\0';
	if (!input || input[0] == '\0')
		return false;

	parse_source_endpoint(input, out_name, name_sz, out_addr, addr_sz);
	if (out_addr[0] != '\0')
		return true;
	if (out_name[0] != '\0')
		return lookup_discovered_endpoint(out_name, out_addr, addr_sz);
	return false;
}

static int sanitize_color_format(int cf)
{
	switch (cf) {
	case OAVX_COLOR_FORMAT_BGRX_BGRA:
	case OAVX_COLOR_FORMAT_UYVY_BGRA:
	case OAVX_COLOR_FORMAT_RGBX_RGBA:
	case OAVX_COLOR_FORMAT_UYVY_RGBA:
	case OAVX_COLOR_FORMAT_BEST:
		return cf;
	case OAVX_COLOR_FORMAT_FASTEST:
	case OAVX_COLOR_FORMAT_COMPRESSED_V1:
	default:
		/* Raw compressed passthrough cannot be rendered as async OBS video frames */
		return OAVX_COLOR_FORMAT_UYVY_BGRA;
	}
}

static oavx_transport_e sanitize_transport(int transport)
{
	switch (transport) {
	case OAVX_TRANSPORT_TCP:
	case OAVX_TRANSPORT_RUDP:
	case OAVX_TRANSPORT_UDP:
		return (oavx_transport_e)transport;
	default:
		return OAVX_TRANSPORT_TCP;
	}
}

static enum video_format oavx_to_obs_video_format(oavx_fourcc_video_type_e fourcc)
{
	switch (fourcc) {
	case OAVX_FOURCC_VIDEO_TYPE_UYVY:
		return VIDEO_FORMAT_UYVY;
	case OAVX_FOURCC_VIDEO_TYPE_BGRA:
		return VIDEO_FORMAT_BGRA;
	case OAVX_FOURCC_VIDEO_TYPE_RGBA:
		return VIDEO_FORMAT_RGBA;
	case OAVX_FOURCC_VIDEO_TYPE_BGRX:
		return VIDEO_FORMAT_BGRX;
	case OAVX_FOURCC_VIDEO_TYPE_RGBX:
		return VIDEO_FORMAT_RGBA;
	case OAVX_FOURCC_VIDEO_TYPE_NV12:
		return VIDEO_FORMAT_NV12;
	case OAVX_FOURCC_VIDEO_TYPE_I420:
	case OAVX_FOURCC_VIDEO_TYPE_YV12:
		return VIDEO_FORMAT_I420;
	case OAVX_FOURCC_VIDEO_TYPE_P216:
		return VIDEO_FORMAT_P216;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static enum speaker_layout channels_to_obs_speakers(uint16_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_STEREO;
	}
}

static inline uint8_t clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

static bool ensure_scratch_buf(struct openavx_source *src, size_t needed)
{
	if (needed == 0)
		return false;
	if (src->scratch_cap >= needed && src->scratch_buf)
		return true;
	uint8_t *nb = (uint8_t *)brealloc(src->scratch_buf, needed);
	if (!nb)
		return false;
	src->scratch_buf = nb;
	src->scratch_cap = needed;
	return true;
}

/* Fallback UYVA -> BGRA (BT.709 limited-range) preserving alpha if Native/BEST is active */
static bool convert_uyva_to_bgra(struct openavx_source *src, const oavx_video_frame_t *vf)
{
	uint32_t w = vf->width;
	uint32_t h = vf->height;
	if (w == 0 || h == 0 || !vf->planes[0])
		return false;

	size_t needed = (size_t)w * (size_t)h * 4;
	if (!ensure_scratch_buf(src, needed))
		return false;

	size_t yuv_stride = vf->strides[0] > 0 ? vf->strides[0] : (size_t)w * 2;
	const uint8_t *alpha_base = vf->planes[1] ? vf->planes[1] : (vf->planes[0] + (size_t)h * yuv_stride);
	size_t alpha_stride = (vf->planes[1] && vf->strides[1] > 0) ? vf->strides[1] : (size_t)w;

	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *row_yuv = vf->planes[0] + (size_t)y * yuv_stride;
		const uint8_t *row_a = alpha_base + (size_t)y * alpha_stride;
		uint8_t *dst = src->scratch_buf + (size_t)y * (size_t)w * 4;

		for (uint32_t x = 0; x + 1 < w; x += 2) {
			int u = (int)row_yuv[0] - 128;
			int y0 = (int)row_yuv[1] - 16;
			int v = (int)row_yuv[2] - 128;
			int y1 = (int)row_yuv[3] - 16;
			row_yuv += 4;

			int c0 = 298 * y0;
			int c1 = 298 * y1;
			int d_b = 541 * u;
			int d_g = -54 * u - 137 * v;
			int d_r = 459 * v;

			dst[0] = clamp_u8((c0 + d_b + 128) >> 8);
			dst[1] = clamp_u8((c0 + d_g + 128) >> 8);
			dst[2] = clamp_u8((c0 + d_r + 128) >> 8);
			dst[3] = row_a[x];

			dst[4] = clamp_u8((c1 + d_b + 128) >> 8);
			dst[5] = clamp_u8((c1 + d_g + 128) >> 8);
			dst[6] = clamp_u8((c1 + d_r + 128) >> 8);
			dst[7] = row_a[x + 1];
			dst += 8;
		}
	}
	return true;
}

/* Ensure RGBX always displays opaque even if source sets X = 0x00 */
static bool convert_rgbx_to_opaque_rgba(struct openavx_source *src, const oavx_video_frame_t *vf)
{
	uint32_t w = vf->width;
	uint32_t h = vf->height;
	if (w == 0 || h == 0 || !vf->planes[0])
		return false;

	size_t dst_stride = (size_t)w * 4;
	size_t needed = dst_stride * (size_t)h;
	if (!ensure_scratch_buf(src, needed))
		return false;

	size_t src_stride = vf->strides[0] > 0 ? vf->strides[0] : dst_stride;
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *sp = vf->planes[0] + (size_t)y * src_stride;
		uint8_t *dp = src->scratch_buf + (size_t)y * dst_stride;
		memcpy(dp, sp, dst_stride);
		for (uint32_t x = 0; x < w; ++x) {
			dp[(size_t)x * 4 + 3] = 0xFF;
		}
	}
	return true;
}

static uint64_t map_remote_pts_to_obs_ns(struct openavx_source *src, int64_t remote_pts_ns, uint64_t now_ns)
{
	if (remote_pts_ns <= 0)
		return now_ns;

	if (!src->has_pts_offset) {
		src->pts_offset_ns = (int64_t)now_ns - remote_pts_ns;
		src->has_pts_offset = true;
		src->last_pts_anchor_ns = now_ns;
		return now_ns;
	}

	int64_t mapped = remote_pts_ns + src->pts_offset_ns;
	int64_t drift = mapped - (int64_t)now_ns;
	if (drift < 0)
		drift = -drift;

	/* Re-anchor cleanly on stream restart, seek, or excessive clock divergence (> 200 ms) */
	if (mapped <= 0 || drift > 200000000LL) {
		src->pts_offset_ns = (int64_t)now_ns - remote_pts_ns;
		src->last_pts_anchor_ns = now_ns;
		return now_ns;
	}

	return (uint64_t)mapped;
}

static void emit_video_frame_to_obs(struct openavx_source *src, const oavx_video_frame_t *vf, uint64_t timestamp_ns,
				    bool *warned_compressed)
{
	if (!vf || !vf->planes[0] || vf->width == 0 || vf->height == 0)
		return;

	struct obs_source_frame2 obs_vf;
	memset(&obs_vf, 0, sizeof(obs_vf));

	enum video_format fmt = VIDEO_FORMAT_NONE;
	if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
		if (!convert_uyva_to_bgra(src, vf))
			return;
		fmt = VIDEO_FORMAT_BGRA;
		obs_vf.data[0] = src->scratch_buf;
		obs_vf.linesize[0] = vf->width * 4;
	} else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
		if (!convert_rgbx_to_opaque_rgba(src, vf))
			return;
		fmt = VIDEO_FORMAT_RGBA;
		obs_vf.data[0] = src->scratch_buf;
		obs_vf.linesize[0] = vf->width * 4;
	} else {
		fmt = oavx_to_obs_video_format(vf->fourcc);
		if (fmt == VIDEO_FORMAT_NONE) {
			if (warned_compressed && !*warned_compressed) {
				obs_log(LOG_WARNING,
					"[source] unsupported or undecoded video fourcc 0x%08X "
					"(ensure runtime codec library or Auto color format)",
					(unsigned)vf->fourcc);
				*warned_compressed = true;
			}
			return;
		}
		if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
			obs_vf.data[0] = vf->planes[0];
			obs_vf.linesize[0] = (uint32_t)vf->strides[0];
			obs_vf.data[1] = vf->planes[2];
			obs_vf.linesize[1] = (uint32_t)vf->strides[2];
			obs_vf.data[2] = vf->planes[1];
			obs_vf.linesize[2] = (uint32_t)vf->strides[1];
		} else {
			for (int i = 0; i < 4; ++i) {
				obs_vf.data[i] = vf->planes[i];
				obs_vf.linesize[i] = (uint32_t)vf->strides[i];
			}
		}
	}

	bool is_yuv = format_is_yuv(fmt);
	enum video_colorspace cs = is_yuv ? VIDEO_CS_709 : VIDEO_CS_SRGB;
	enum video_range_type rng = is_yuv ? VIDEO_RANGE_PARTIAL : VIDEO_RANGE_FULL;
	uint8_t trc = is_yuv ? VIDEO_TRC_DEFAULT : VIDEO_TRC_SRGB;

	avx_color_info_t hdr_info;
	if (is_yuv && vf->metadata && avx_hdr_frame_get_color_info(vf, &hdr_info)) {
		if (hdr_info.transfer == AVX_COLOR_TRANSFER_BT2100_PQ) {
			cs = VIDEO_CS_2100_PQ;
			trc = VIDEO_TRC_PQ;
		} else if (hdr_info.transfer == AVX_COLOR_TRANSFER_BT2100_HLG) {
			cs = VIDEO_CS_2100_HLG;
			trc = VIDEO_TRC_HLG;
		}
	}

	pthread_mutex_lock(&src->cfg_lock);
	int user_cr = src->color_range;
	int user_cs = src->color_space;
	pthread_mutex_unlock(&src->cfg_lock);

	if (user_cr == OPENAVX_COLOR_RANGE_PARTIAL) {
		rng = VIDEO_RANGE_PARTIAL;
	} else if (user_cr == OPENAVX_COLOR_RANGE_FULL) {
		rng = VIDEO_RANGE_FULL;
	}

	if (user_cs == OPENAVX_COLOR_SPACE_BT709) {
		cs = VIDEO_CS_709;
		trc = VIDEO_TRC_DEFAULT;
	} else if (user_cs == OPENAVX_COLOR_SPACE_BT601) {
		cs = VIDEO_CS_601;
		trc = VIDEO_TRC_DEFAULT;
	} else if (user_cs == OPENAVX_COLOR_SPACE_BT2100_PQ) {
		cs = VIDEO_CS_2100_PQ;
		trc = VIDEO_TRC_PQ;
	} else if (user_cs == OPENAVX_COLOR_SPACE_BT2100_HLG) {
		cs = VIDEO_CS_2100_HLG;
		trc = VIDEO_TRC_HLG;
	}

	obs_vf.format = fmt;
	obs_vf.width = vf->width;
	obs_vf.height = vf->height;
	obs_vf.timestamp = timestamp_ns;
	obs_vf.range = rng;
	obs_vf.trc = trc;
	video_format_get_parameters_for_format(cs, rng, fmt, obs_vf.color_matrix, obs_vf.color_range_min,
					       obs_vf.color_range_max);

	os_atomic_store_long(&src->width, (long)vf->width);
	os_atomic_store_long(&src->height, (long)vf->height);
	obs_source_output_video2(src->source, &obs_vf);
	src->video_frames++;
}

static void emit_audio_frame_to_obs(struct openavx_source *src, const oavx_audio_frame_t *af, uint64_t timestamp_ns)
{
	if (!af || !af->planes[0] || af->frames == 0 || af->sample_rate == 0 || af->channels == 0)
		return;

	struct obs_source_audio obs_af;
	memset(&obs_af, 0, sizeof(obs_af));
	obs_af.frames = af->frames;
	obs_af.samples_per_sec = af->sample_rate;
	obs_af.timestamp = timestamp_ns;
	obs_af.speakers = channels_to_obs_speakers(af->channels);
	size_t obs_ch = get_audio_channels(obs_af.speakers);

	switch (af->format) {
	case OAVX_AUDIO_FMT_FLT_INTERLEAVED:
		obs_af.format = AUDIO_FORMAT_FLOAT;
		obs_af.data[0] = (const uint8_t *)af->planes[0];
		break;
	case OAVX_AUDIO_FMT_S16_INTERLEAVED:
		obs_af.format = AUDIO_FORMAT_16BIT;
		obs_af.data[0] = (const uint8_t *)af->planes[0];
		break;
	case OAVX_AUDIO_FMT_S32_INTERLEAVED:
		obs_af.format = AUDIO_FORMAT_32BIT;
		obs_af.data[0] = (const uint8_t *)af->planes[0];
		break;
	case OAVX_AUDIO_FMT_FLT_PLANAR:
	default:
		obs_af.format = AUDIO_FORMAT_FLOAT_PLANAR;
		for (size_t i = 0; i < obs_ch && i < (size_t)af->channels && i < MAX_AV_PLANES; ++i) {
			obs_af.data[i] = (const uint8_t *)af->planes[i];
		}
		break;
	}

	obs_source_output_audio(src->source, &obs_af);
	src->audio_frames++;
}

static void redact_endpoint_secrets(const char *endpoint, char *out, size_t cap)
{
	if (!out || cap == 0)
		return;
	if (!endpoint || !endpoint[0]) {
		out[0] = '\0';
		return;
	}
	size_t wi = 0;
	const char *p = endpoint;
	while (*p && wi + 1 < cap) {
		if ((p == endpoint || p[-1] == '?' || p[-1] == '&') && strncmp(p, "key=", 4) == 0) {
			const char *rep = "key=***";
			for (size_t k = 0; rep[k] && wi + 1 < cap; ++k)
				out[wi++] = rep[k];
			p += 4;
			while (*p && *p != '&')
				p++;
			continue;
		}
		out[wi++] = *p++;
	}
	out[wi] = '\0';
}

static void update_source_telem_status(struct openavx_source *src, oavx_stream_t *stream, const char *endpoint,
				       bool connected)
{
	if (!src)
		return;
	char safe_ep[256];
	redact_endpoint_secrets(endpoint, safe_ep, sizeof(safe_ep));

	char buf[256];
	if (connected && stream) {
		oavx_telemetry_t telem;
		memset(&telem, 0, sizeof(telem));
		oavx_stream_get_telemetry(stream, &telem);
		uint32_t w = (uint32_t)os_atomic_load_long(&src->width);
		uint32_t h = (uint32_t)os_atomic_load_long(&src->height);
		bool on_failover = oavx_stream_is_on_failover(stream);
		bool rec_active = false;
		oavx_stream_record_status(stream, &rec_active, NULL, NULL);
		if (w > 0 && h > 0) {
			snprintf(buf, sizeof(buf), "Connected (%ux%u, %.1f Mbps, RTT: %.1f ms)%s%s", w, h,
				 (double)telem.bitrate_mbps, (double)telem.kernel_rtt_ms,
				 on_failover ? " [FAILOVER]" : "", rec_active ? " [REC]" : "");
		} else if (src->bandwidth == OAVX_BANDWIDTH_AUDIO_ONLY && src->audio_frames > 0) {
			snprintf(buf, sizeof(buf), "Connected (Audio Only, %.1f Mbps, RTT: %.1f ms)%s%s",
				 (double)telem.bitrate_mbps, (double)telem.kernel_rtt_ms,
				 on_failover ? " [FAILOVER]" : "", rec_active ? " [REC]" : "");
		} else if (src->video_frames == 0 && src->audio_frames == 0) {
			snprintf(buf, sizeof(buf), "Waiting for media from %s...%s%s", safe_ep[0] ? safe_ep : "Stream",
				 on_failover ? " [FAILOVER]" : "", rec_active ? " [REC]" : "");
		} else {
			snprintf(buf, sizeof(buf), "Connected to %s%s%s", safe_ep[0] ? safe_ep : "Stream",
				 on_failover ? " [FAILOVER]" : "", rec_active ? " [REC]" : "");
		}
	} else if (safe_ep[0]) {
		snprintf(buf, sizeof(buf), "Connecting to %s...", safe_ep);
	} else {
		snprintf(buf, sizeof(buf), "Waiting for stream discovery...");
	}
	pthread_mutex_lock(&src->telem_lock);
	snprintf(src->telem_status, sizeof(src->telem_status), "%s", buf);
	pthread_mutex_unlock(&src->telem_lock);
}

static void update_tally_from_obs(struct openavx_source *src);

static void sync_tally_if_needed(struct openavx_source *src, oavx_stream_t *stream, long *last_sent_tally)
{
	if (!src || !stream || !last_sent_tally)
		return;

	uint64_t now_ns = os_gettime_ns();
	if (now_ns - src->last_tally_eval_ns >= 100000000ULL) { /* 100 ms periodic refresh */
		src->last_tally_eval_ns = now_ns;
		update_tally_from_obs(src);
	}

	long cur_tally = os_atomic_load_long(&src->desired_tally);
	if (cur_tally != *last_sent_tally) {
		oavx_tally_t t = {.flags = (uint32_t)cur_tally};
		if (oavx_stream_set_tally(stream, &t)) {
			*last_sent_tally = cur_tally;
		}
	}
}

static void process_pending_remote_commands(struct openavx_source *src, oavx_stream_t *stream)
{
	if (!src || !stream)
		return;

	bool do_ptz = false;
	oavx_ptz_state_t ptz;
	memset(&ptz, 0, sizeof(ptz));
	int rec_cmd = 0;
	char rec_hint[256] = {0};

	pthread_mutex_lock(&src->cfg_lock);
	if (src->ptz_pending) {
		do_ptz = true;
		ptz.pan = (float)src->ptz_pan;
		ptz.tilt = (float)src->ptz_tilt;
		ptz.zoom = (float)src->ptz_zoom;
		ptz.focus = (float)src->ptz_focus;
		src->ptz_pending = false;
	}
	if (src->record_cmd_pending != 0) {
		rec_cmd = src->record_cmd_pending;
		src->record_cmd_pending = 0;
		if (src->record_filename && src->record_filename[0])
			snprintf(rec_hint, sizeof(rec_hint), "%s", src->record_filename);
	}
	pthread_mutex_unlock(&src->cfg_lock);

	if (do_ptz) {
		if (!oavx_stream_set_ptz(stream, &ptz)) {
			obs_log(LOG_WARNING, "[source] failed to send PTZ command");
		}
	}
	if (rec_cmd == 1) {
		if (!oavx_stream_record_trigger(stream, "start", rec_hint[0] ? rec_hint : NULL)) {
			obs_log(LOG_WARNING, "[source] failed to trigger remote recording start");
		}
	} else if (rec_cmd == 2) {
		if (!oavx_stream_record_trigger(stream, "stop", NULL)) {
			obs_log(LOG_WARNING, "[source] failed to trigger remote recording stop");
		}
	}
}

static void clear_obs_output(struct openavx_source *src)
{
	obs_source_output_video(src->source, NULL);
	os_atomic_store_long(&src->width, 0);
	os_atomic_store_long(&src->height, 0);
}

static void sleep_interruptible(struct openavx_source *src, uint32_t ms)
{
	uint32_t elapsed = 0;
	while (elapsed < ms && os_atomic_load_bool(&src->running)) {
		uint32_t step = (ms - elapsed) > 25 ? 25 : (ms - elapsed);
		os_sleep_ms(step);
		elapsed += step;
	}
}

static void *openavx_recv_worker(void *param)
{
	struct openavx_source *src = (struct openavx_source *)param;

	pthread_mutex_lock(&src->cfg_lock);
	char *configured_name = bstrdup(src->source_name ? src->source_name : "");
	char *configured_pass = bstrdup(src->passphrase ? src->passphrase : "");
	int bandwidth = src->bandwidth;
	int color_format = sanitize_color_format(src->color_format);
	int buffer_mode = src->buffer_mode;
	int behavior = src->behavior;
	oavx_transport_e transport = sanitize_transport(src->transport);
	bool clear_on_disc = src->clear_on_disconnect;
	char *configured_failover = bstrdup(src->failover_source ? src->failover_source : "");
	int failover_timeout_ms = src->failover_timeout_ms > 0 ? src->failover_timeout_ms : 2000;
	bool auto_revert = src->auto_revert;
	int auto_revert_hold_ms = src->auto_revert_hold_ms > 0 ? src->auto_revert_hold_ms : 3000;
	pthread_mutex_unlock(&src->cfg_lock);

	char parsed_name[256] = {0};
	char parsed_ep[256] = {0};
	parse_source_endpoint(configured_name, parsed_name, sizeof(parsed_name), parsed_ep, sizeof(parsed_ep));

	obs_log(LOG_INFO, "[source] worker started for '%s' (mode=%s, e2ee=%s)",
		configured_name[0] ? configured_name : "(none)",
		buffer_mode == OPENAVX_BUFFER_FRAMESYNC ? "FrameSync" : "LowLatency",
		(configured_pass && configured_pass[0]) ? "yes" : "no");

	oavx_stream_t *stream = NULL;
	oavx_framesync_t *fs = NULL;
	long last_sent_tally = -1;
	bool warned_waiting = false;
	bool warned_compressed = false;
	bool was_connected = false;
	uint64_t next_fs_tick_ns = 0;
	double audio_sample_acc = 0.0;
	uint64_t last_telem_update_ns = 0;
	char resolved_ep[256] = {0};

	while (os_atomic_load_bool(&src->running)) {
		/* Pause/disconnect when hidden if requested by user */
		if (behavior == OPENAVX_BEHAVIOR_STOP_HIDDEN && os_atomic_load_bool(&src->obs_hidden)) {
			if (fs) {
				oavx_framesync_destroy(fs);
				fs = NULL;
			}
			if (stream) {
				oavx_stream_close(stream);
				stream = NULL;
				if (clear_on_disc)
					clear_obs_output(src);
			}
			was_connected = false;
			last_sent_tally = -1;
			src->has_pts_offset = false;
			update_source_telem_status(src, NULL, parsed_name, false);
			sleep_interruptible(src, 100);
			continue;
		}

		if (configured_name[0] == '\0') {
			update_source_telem_status(src, NULL, "", false);
			sleep_interruptible(src, 100);
			continue;
		}

		/* Resolve & open stream inside background worker (never blocks OBS UI) */
		if (!stream) {
			resolved_ep[0] = '\0';
			if (parsed_ep[0] != '\0') {
				snprintf(resolved_ep, sizeof(resolved_ep), "%s", parsed_ep);
			} else if (!lookup_discovered_endpoint(parsed_name, resolved_ep, sizeof(resolved_ep))) {
				if (!warned_waiting) {
					obs_log(LOG_INFO, "[source] waiting to discover OpenAVX stream '%s'...",
						parsed_name);
					warned_waiting = true;
				}
				update_source_telem_status(src, NULL, parsed_name, false);
				if (g_disc_wake)
					os_event_signal(g_disc_wake);
				sleep_interruptible(src, 300);
				continue;
			}

			oavx_stream_config_t cfg;
			memset(&cfg, 0, sizeof(cfg));
			cfg.name = parsed_name[0] ? parsed_name : "OpenAVX Source";
			cfg.endpoint = resolved_ep;
			cfg.direction = OAVX_DIR_RECEIVE;
			cfg.bandwidth = (oavx_bandwidth_e)bandwidth;
			cfg.color_format = (oavx_color_format_e)color_format;
			cfg.transport = transport;
			cfg.clock_sync = false;
			cfg.passphrase = (configured_pass && configured_pass[0]) ? configured_pass : NULL;

			obs_log(LOG_INFO, "[source] connecting to '%s' (%s, transport=%d, e2ee=%s)", cfg.name,
				cfg.endpoint, (int)transport, cfg.passphrase ? "active" : "disabled");
			update_source_telem_status(src, NULL, resolved_ep, false);
			stream = oavx_stream_open(src->ctx, &cfg);
			if (!stream) {
				update_source_telem_status(src, NULL, resolved_ep, false);
				sleep_interruptible(src, 500);
				continue;
			}

			if (configured_failover && configured_failover[0] != '\0') {
				char fo_name[256] = {0};
				char fo_addr[256] = {0};
				if (openavx_source_resolve_endpoint(configured_failover, fo_name, sizeof(fo_name),
								    fo_addr, sizeof(fo_addr))) {
					oavx_endpoint_t fep;
					memset(&fep, 0, sizeof(fep));
					fep.name = fo_name[0] ? fo_name : "OpenAVX Failover";
					fep.address = fo_addr;
					oavx_stream_set_failover(stream, &fep);
					obs_log(LOG_INFO, "[source] failover configured -> '%s' (%s)", fep.name,
						fep.address);
				} else {
					obs_log(LOG_WARNING,
						"[source] failover target '%s' not resolved yet; relying on sender metadata if advertised",
						configured_failover);
				}
			}
			oavx_stream_set_failover_timeout(stream, (uint32_t)failover_timeout_ms);
			oavx_stream_set_auto_revert(stream, auto_revert, (uint32_t)auto_revert_hold_ms);

			warned_waiting = false;
			warned_compressed = false;
			last_sent_tally = -1;
			src->has_pts_offset = false;
			update_source_telem_status(src, stream, resolved_ep, true);

			if (bandwidth == OAVX_BANDWIDTH_AUDIO_ONLY)
				clear_obs_output(src);

			if (buffer_mode == OPENAVX_BUFFER_FRAMESYNC) {
				fs = oavx_framesync_create(stream);
				next_fs_tick_ns = os_gettime_ns();
				audio_sample_acc = 0.0;
			}
		}

		process_pending_remote_commands(src, stream);

		if (buffer_mode == OPENAVX_BUFFER_FRAMESYNC && fs) {
			uint32_t fps_num = 60000;
			uint32_t fps_den = 1001;
			struct obs_video_info ovi;
			if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0) {
				fps_num = ovi.fps_num;
				fps_den = ovi.fps_den;
			}
			uint64_t tick_ns = ((uint64_t)fps_den * 1000000000ULL) / (uint64_t)fps_num;
			if (tick_ns < 4000000ULL)
				tick_ns = 4000000ULL;

			uint32_t sample_rate = 48000;
			uint16_t channels = 2;
			audio_t *obs_audio = obs_get_audio();
			if (obs_audio) {
				uint32_t sr = audio_output_get_sample_rate(obs_audio);
				size_t ch = audio_output_get_channels(obs_audio);
				if (sr > 0)
					sample_rate = sr;
				if (ch > 0 && ch <= 8)
					channels = (uint16_t)ch;
			}

			uint64_t now_ns = os_gettime_ns();
			if (next_fs_tick_ns == 0 || now_ns > next_fs_tick_ns + tick_ns * 4)
				next_fs_tick_ns = now_ns;

			oavx_video_frame_t vf;
			memset(&vf, 0, sizeof(vf));
			oavx_framesync_capture_video(fs, &vf, 0);
			if (vf.planes[0] && bandwidth != OAVX_BANDWIDTH_AUDIO_ONLY) {
				emit_video_frame_to_obs(src, &vf, now_ns, &warned_compressed);
			}
			oavx_framesync_free_video(fs, &vf);

			oavx_telemetry_t telem;
			memset(&telem, 0, sizeof(telem));
			if (oavx_stream_get_telemetry(stream, &telem)) {
				if (telem.is_connected) {
					if (!was_connected) {
						was_connected = true;
						obs_log(LOG_INFO, "[source] connected to '%s' (%s)", parsed_name,
							telem.remote_endpoint);
					}
					sync_tally_if_needed(src, stream, &last_sent_tally);
				} else if (was_connected) {
					was_connected = false;
					obs_log(LOG_WARNING, "[source] connection lost to '%s'; reconnecting",
						parsed_name);
					oavx_framesync_destroy(fs);
					fs = NULL;
					oavx_stream_close(stream);
					stream = NULL;
					last_sent_tally = -1;
					if (clear_on_disc)
						clear_obs_output(src);
					sleep_interruptible(src, 300);
					continue;
				}

				if (telem.audio_frames_total > 0) {
					audio_sample_acc += ((double)tick_ns * (double)sample_rate) / 1000000000.0;
					uint32_t target_samples = (uint32_t)llround(audio_sample_acc);
					audio_sample_acc -= (double)target_samples;
					if (target_samples > 0 && target_samples <= 4800) {
						oavx_audio_frame_t af;
						memset(&af, 0, sizeof(af));
						oavx_framesync_capture_audio(fs, &af, sample_rate, channels,
									     target_samples);
						emit_audio_frame_to_obs(src, &af, now_ns);
						oavx_framesync_free_audio(fs, &af);
					}
				}
			}

			next_fs_tick_ns += tick_ns;
			now_ns = os_gettime_ns();
			if (next_fs_tick_ns > now_ns) {
				uint32_t wait_ms = (uint32_t)((next_fs_tick_ns - now_ns) / 1000000ULL);
				if (wait_ms > 0)
					sleep_interruptible(src, wait_ms);
			}
			continue;
		}

		/* Low-latency direct receive path */
		oavx_video_frame_t vframe;
		oavx_audio_frame_t aframe;
		oavx_metadata_frame_t mframe;
		memset(&vframe, 0, sizeof(vframe));
		memset(&aframe, 0, sizeof(aframe));
		memset(&mframe, 0, sizeof(mframe));

		oavx_frame_type_t ft = oavx_stream_receive(stream, &vframe, &aframe, &mframe, 40);
		if (!os_atomic_load_bool(&src->running)) {
			if (ft == OAVX_FRAME_TYPE_VIDEO)
				oavx_stream_release_video(stream, &vframe);
			if (ft == OAVX_FRAME_TYPE_AUDIO)
				oavx_stream_release_audio(stream, &aframe);
			if (ft == OAVX_FRAME_TYPE_METADATA)
				oavx_stream_release_metadata(stream, &mframe);
			break;
		}

		if (ft == OAVX_FRAME_TYPE_ERROR) {
			if (oavx_stream_is_on_failover(stream)) {
				obs_log(LOG_INFO, "[source] primary '%s' disconnected; switched to failover stream",
					parsed_name);
				last_sent_tally = -1;
				src->has_pts_offset = false;
				update_source_telem_status(src, stream, resolved_ep, true);
				sleep_interruptible(src, 20);
				continue;
			}
			if (was_connected) {
				obs_log(LOG_WARNING, "[source] stream disconnected from '%s'; re-resolving",
					parsed_name);
				was_connected = false;
			}
			update_source_telem_status(src, NULL, parsed_name, false);
			oavx_stream_close(stream);
			stream = NULL;
			last_sent_tally = -1;
			src->has_pts_offset = false;
			if (clear_on_disc)
				clear_obs_output(src);
			sleep_interruptible(src, 300);
			continue;
		}

		if (ft == OAVX_FRAME_TYPE_VIDEO || ft == OAVX_FRAME_TYPE_AUDIO || ft == OAVX_FRAME_TYPE_METADATA) {
			if (!was_connected) {
				was_connected = true;
				obs_log(LOG_INFO, "[source] receiving stream '%s'", parsed_name);
				update_source_telem_status(src, stream, resolved_ep, true);
			}
			sync_tally_if_needed(src, stream, &last_sent_tally);
		}

		uint64_t now_ns = os_gettime_ns();
		if (now_ns - last_telem_update_ns > 1000000000ULL) {
			last_telem_update_ns = now_ns;
			if (stream && was_connected) {
				update_source_telem_status(src, stream, resolved_ep, true);
			}
		}

		if (ft == OAVX_FRAME_TYPE_VIDEO) {
			if (bandwidth != OAVX_BANDWIDTH_AUDIO_ONLY) {
				uint64_t ts = map_remote_pts_to_obs_ns(src, vframe.pts_ns, now_ns);
				emit_video_frame_to_obs(src, &vframe, ts, &warned_compressed);
			}
			oavx_stream_release_video(stream, &vframe);
		} else if (ft == OAVX_FRAME_TYPE_AUDIO) {
			uint64_t ts = map_remote_pts_to_obs_ns(src, aframe.pts_ns, now_ns);
			emit_audio_frame_to_obs(src, &aframe, ts);
			oavx_stream_release_audio(stream, &aframe);
		} else if (ft == OAVX_FRAME_TYPE_METADATA) {
			oavx_stream_release_metadata(stream, &mframe);
		}
	}

	if (fs)
		oavx_framesync_destroy(fs);
	if (stream)
		oavx_stream_close(stream);
	bfree(configured_failover);
	bfree(configured_pass);
	bfree(configured_name);

	obs_log(LOG_INFO, "[source] worker terminated (frames: video=%" PRIu64 ", audio=%" PRIu64 ")",
		src->video_frames, src->audio_frames);
	return NULL;
}

static void openavx_source_start(struct openavx_source *src)
{
	if (src->thread_started)
		return;

	pthread_mutex_lock(&src->cfg_lock);
	bool has_name = (src->source_name && src->source_name[0] != '\0');
	pthread_mutex_unlock(&src->cfg_lock);

	if (!has_name)
		return;

	os_atomic_store_bool(&src->running, true);
	if (pthread_create(&src->recv_thread, NULL, openavx_recv_worker, src) == 0) {
		src->thread_started = true;
	} else {
		os_atomic_store_bool(&src->running, false);
		obs_log(LOG_ERROR, "[source] failed to spawn receiver worker thread");
	}
}

static void openavx_source_stop(struct openavx_source *src)
{
	if (src->thread_started) {
		os_atomic_store_bool(&src->running, false);
		pthread_join(src->recv_thread, NULL);
		src->thread_started = false;
	}
}

static bool parse_http_url(const char *url, char *host_out, size_t host_sz, uint16_t *port_out, char *path_out,
			   size_t path_sz)
{
	if (!url || !url[0])
		return false;

	const char *p = url;
	bool is_https = false;
	if (strncmp(p, "http://", 7) == 0) {
		p += 7;
	} else if (strncmp(p, "https://", 8) == 0) {
		p += 8;
		is_https = true;
	}

	const char *slash = strchr(p, '/');
	const char *colon = strchr(p, ':');

	if (colon && (!slash || colon < slash)) {
		size_t hlen = (size_t)(colon - p);
		if (hlen >= host_sz)
			hlen = host_sz - 1;
		memcpy(host_out, p, hlen);
		host_out[hlen] = '\0';
		*port_out = (uint16_t)atoi(colon + 1);
	} else if (slash) {
		size_t hlen = (size_t)(slash - p);
		if (hlen >= host_sz)
			hlen = host_sz - 1;
		memcpy(host_out, p, hlen);
		host_out[hlen] = '\0';
		*port_out = is_https ? 443 : 80;
	} else {
		snprintf(host_out, host_sz, "%s", p);
		*port_out = is_https ? 443 : 80;
	}

	if (slash) {
		snprintf(path_out, path_sz, "%s", slash);
	} else {
		snprintf(path_out, path_sz, "/api/v1/relay/tally");
	}
	return true;
}

static void extract_cloud_params_from_url(const char *url, char *out_room, size_t room_sz, char *out_key, size_t key_sz,
					  int *out_slot, char *out_tally_url, size_t tally_url_sz)
{
	if (!url || !url[0])
		return;

	/* Extract room / roomSlug */
	const char *r_ptr = strstr(url, "room=");
	if (!r_ptr)
		r_ptr = strstr(url, "roomSlug=");
	if (r_ptr && out_room && (!out_room[0])) {
		const char *val = strchr(r_ptr, '=') + 1;
		size_t len = 0;
		while (val[len] && val[len] != '&' && val[len] != ' ' && val[len] != '#' && len + 1 < room_sz) {
			len++;
		}
		memcpy(out_room, val, len);
		out_room[len] = '\0';
	}

	/* Extract key / streamKey */
	const char *k_ptr = strstr(url, "key=");
	if (!k_ptr)
		k_ptr = strstr(url, "streamKey=");
	if (k_ptr && out_key && (!out_key[0])) {
		const char *val = strchr(k_ptr, '=') + 1;
		size_t len = 0;
		while (val[len] && val[len] != '&' && val[len] != ' ' && val[len] != '#' && len + 1 < key_sz) {
			len++;
		}
		memcpy(out_key, val, len);
		out_key[len] = '\0';
	}

	/* Extract slot / slotIndex */
	const char *s_ptr = strstr(url, "slot=");
	if (!s_ptr)
		s_ptr = strstr(url, "slotIndex=");
	if (s_ptr && out_slot && *out_slot <= 1) {
		const char *val = strchr(s_ptr, '=') + 1;
		int s = atoi(val);
		if (s > 0)
			*out_slot = s;
	}

	/* Infer Cloud Tally URL from openavx:// URL if empty */
	if (out_tally_url && (!out_tally_url[0]) && strncmp(url, "openavx://", 10) == 0) {
		const char *host_start = url + 10;
		const char *slash = strchr(host_start, '/');
		const char *colon = strchr(host_start, ':');
		char host[128] = {0};
		if (colon && (!slash || colon < slash)) {
			size_t hlen = (size_t)(colon - host_start);
			if (hlen >= sizeof(host))
				hlen = sizeof(host) - 1;
			memcpy(host, host_start, hlen);
			host[hlen] = '\0';
		} else if (slash) {
			size_t hlen = (size_t)(slash - host_start);
			if (hlen >= sizeof(host))
				hlen = sizeof(host) - 1;
			memcpy(host, host_start, hlen);
			host[hlen] = '\0';
		} else {
			snprintf(host, sizeof(host), "%s", host_start);
		}

		if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
			snprintf(out_tally_url, tally_url_sz, "http://%s:3001/api/v1/relay/tally", host);
		} else {
			snprintf(out_tally_url, tally_url_sz, "http://%s:3001/api/v1/relay/tally", host);
		}
	}
}

static bool send_cloud_tally_http(const char *url, const char *room, const char *key, int slot, bool is_prog,
				  bool is_prev, bool is_rec)
{
	if (!url || !url[0] || !room || !room[0])
		return false;

	char host[128] = {0};
	char path[256] = {0};
	uint16_t port = 3001;
	if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)))
		return false;

	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%u", port);

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
		return false;
	}

	int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		freeaddrinfo(res);
		return false;
	}

#if defined(_WIN32)
	DWORD timeout_ms = 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)&tv, sizeof(tv));
#endif

	if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) < 0) {
#if defined(_WIN32)
		closesocket(fd);
#else
		close(fd);
#endif
		freeaddrinfo(res);
		return false;
	}
	freeaddrinfo(res);

	char body[512];
	snprintf(body, sizeof(body),
		 "{\"room\":\"%s\",\"key\":\"%s\",\"slotIndex\":%d,"
		 "\"isProgram\":%s,\"isPreview\":%s,\"isRecording\":%s,\"exclusive\":true}",
		 room, key ? key : "", slot > 0 ? slot : 1, is_prog ? "true" : "false", is_prev ? "true" : "false",
		 is_rec ? "true" : "false");

	char req[1024];
	int req_len = snprintf(req, sizeof(req),
			       "POST %s HTTP/1.1\r\n"
			       "Host: %s:%u\r\n"
			       "User-Agent: openavx-obs/0.1.0\r\n"
			       "Content-Type: application/json\r\n"
			       "Content-Length: %zu\r\n"
			       "Connection: close\r\n\r\n"
			       "%s",
			       path, host, port, strlen(body), body);

	ssize_t sent = send(fd, req, (size_t)req_len, 0);
	if (sent != (ssize_t)req_len) {
#if defined(_WIN32)
		closesocket(fd);
#else
		close(fd);
#endif
		return false;
	}

	char resp[512] = {0};
	ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
#if defined(_WIN32)
	closesocket(fd);
#else
	close(fd);
#endif

	if (n > 0 && strstr(resp, "200 OK") != NULL) {
		return true;
	}
	return false;
}

static void *openavx_cloud_tally_worker(void *param)
{
	struct openavx_source *src = (struct openavx_source *)param;
	if (!src)
		return NULL;

	while (os_atomic_load_bool(&src->cloud_running)) {
		os_event_wait(src->cloud_wake);
		if (!os_atomic_load_bool(&src->cloud_running))
			break;

		pthread_mutex_lock(&src->cfg_lock);
		bool enabled = src->cloud_tally_enabled;
		char url[256] = {0};
		char room[128] = {0};
		char key[128] = {0};
		int slot = src->cloud_slot;
		if (src->cloud_tally_url)
			snprintf(url, sizeof(url), "%s", src->cloud_tally_url);
		if (src->cloud_room)
			snprintf(room, sizeof(room), "%s", src->cloud_room);
		if (src->cloud_key)
			snprintf(key, sizeof(key), "%s", src->cloud_key);

		if (src->source_name && (!room[0] || !key[0])) {
			extract_cloud_params_from_url(src->source_name, room, sizeof(room), key, sizeof(key), &slot,
						      url, sizeof(url));
		}
		pthread_mutex_unlock(&src->cfg_lock);

		if (!enabled || !room[0])
			continue;

		long target = os_atomic_load_long(&src->cloud_target_tally);
		if (target == src->cloud_last_sent_tally)
			continue;

		bool is_prog = (target & OAVX_TALLY_PROGRAM) != 0;
		bool is_prev = (target & OAVX_TALLY_PREVIEW) != 0;
		bool is_rec = (target & OAVX_TALLY_RECORDING) != 0;

		const char *target_url = url[0] ? url : "http://localhost:3001/api/v1/relay/tally";
		bool ok = send_cloud_tally_http(target_url, room, key, slot > 0 ? slot : 1, is_prog, is_prev, is_rec);
		if (ok) {
			src->cloud_last_sent_tally = target;
			obs_log(LOG_INFO,
				"[source] Cloud tally pushed -> %s (PGM=%d, PVW=%d, REC=%d) for room '%s' slot %d",
				target_url, is_prog, is_prev, is_rec, room, slot > 0 ? slot : 1);
		} else {
			obs_log(LOG_WARNING, "[source] Failed to push cloud tally to %s (room '%s')", target_url, room);
			os_sleep_ms(250);
		}
	}

	return NULL;
}

static const char *openavx_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_SOURCE_NAME;
}

static void update_tally_from_obs(struct openavx_source *src)
{
	if (!src || !src->source)
		return;

	bool active = obs_source_active(src->source);
	bool showing = obs_source_showing(src->source);
	long flags = OAVX_TALLY_NONE;

	if (active) {
		flags |= OAVX_TALLY_PROGRAM;
	}

	/* Disambiguate Studio Mode (Program + Preview dual view) from Normal Mode (Single Program view).
	 * In OBS, obs_source_showing() returns true when active on Program.
	 * Only flag PREVIEW if in Studio Mode and source is visible on preview scene,
	 * OR if the source is showing on screen without being active on Program (!active && showing). */
	bool in_studio_mode = false;
	if (obs_frontend_preview_program_mode_active) {
		in_studio_mode = obs_frontend_preview_program_mode_active();
	}

	if (in_studio_mode) {
		obs_source_t *p_scene_src =
			obs_frontend_get_current_preview_scene ? obs_frontend_get_current_preview_scene() : NULL;
		if (p_scene_src) {
			obs_scene_t *p_scene = obs_scene_from_source(p_scene_src);
			if (p_scene) {
				const char *src_name = obs_source_get_name(src->source);
				obs_sceneitem_t *item = obs_scene_find_source_recursive(p_scene, src_name);
				if (item && obs_sceneitem_visible(item)) {
					flags |= OAVX_TALLY_PREVIEW;
				}
			}
			obs_source_release(p_scene_src);
		} else if (!active && showing) {
			flags |= OAVX_TALLY_PREVIEW;
		}
	} else {
		/* Normal mode: single output. Program (Red) takes precedence.
		 * Preview (Green) is only set if showing without being active on Program (e.g. Multiview). */
		if (!active && showing) {
			flags |= OAVX_TALLY_PREVIEW;
		}
	}

	/* Recording tally: if OBS is actively recording and the source is active or showing */
	if (obs_frontend_recording_active && obs_frontend_recording_active()) {
		if (active || showing) {
			flags |= OAVX_TALLY_RECORDING;
		}
	}

	os_atomic_store_long(&src->desired_tally, flags);
	os_atomic_store_bool(&src->obs_hidden, !active && !showing);

	/* Trigger asynchronous Cloud Tally push */
	long cur_cloud_target = os_atomic_load_long(&src->cloud_target_tally);
	if (cur_cloud_target != flags) {
		os_atomic_store_long(&src->cloud_target_tally, flags);
		if (src->cloud_wake) {
			os_event_signal(src->cloud_wake);
		}
	}
}

static void openavx_source_activate(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	update_tally_from_obs(src);
}

static void openavx_source_deactivate(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	update_tally_from_obs(src);
}

static void openavx_source_show(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	update_tally_from_obs(src);
}

static void openavx_source_hide(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	update_tally_from_obs(src);
}

static void openavx_source_update(void *data, obs_data_t *settings)
{
	struct openavx_source *src = (struct openavx_source *)data;

	const char *new_name = obs_data_get_string(settings, "source_name");
	const char *new_pass = obs_data_get_string(settings, "passphrase");
	int new_bw = (int)obs_data_get_int(settings, "bandwidth");
	int new_cf = sanitize_color_format((int)obs_data_get_int(settings, "color_format"));
	int new_cr = (int)obs_data_get_int(settings, "color_range");
	int new_cs = (int)obs_data_get_int(settings, "color_space");
	int new_bm = (int)obs_data_get_int(settings, "buffer_mode");
	int new_bh = (int)obs_data_get_int(settings, "behavior");
	int new_tr = (int)sanitize_transport((int)obs_data_get_int(settings, "transport"));
	bool new_clear = obs_data_get_bool(settings, "clear_on_disconnect");
	const char *new_failover = obs_data_get_string(settings, "failover_source");
	int new_fo_timeout = (int)obs_data_get_int(settings, "failover_timeout_ms");
	bool new_auto_revert = obs_data_get_bool(settings, "auto_revert");
	int new_revert_hold = (int)obs_data_get_int(settings, "auto_revert_hold_ms");
	const char *new_disc_groups = obs_data_get_string(settings, "discovery_groups");

	double new_ptz_pan = obs_data_get_double(settings, "ptz_pan");
	double new_ptz_tilt = obs_data_get_double(settings, "ptz_tilt");
	double new_ptz_zoom = obs_data_get_double(settings, "ptz_zoom");
	double new_ptz_focus = obs_data_get_double(settings, "ptz_focus");
	const char *new_rec_file = obs_data_get_string(settings, "record_filename");

	bool new_cloud_enabled = obs_data_get_bool(settings, "cloud_tally_enabled");
	const char *new_cloud_url = obs_data_get_string(settings, "cloud_tally_url");
	const char *new_cloud_room = obs_data_get_string(settings, "cloud_room");
	const char *new_cloud_key = obs_data_get_string(settings, "cloud_key");
	int new_cloud_slot = (int)obs_data_get_int(settings, "cloud_slot");

	bool need_restart = false;
	pthread_mutex_lock(&src->cfg_lock);
	bool pass_changed = false;
	if (!src->passphrase && new_pass && new_pass[0]) {
		pass_changed = true;
	} else if (src->passphrase && (!new_pass || strcmp(src->passphrase, new_pass) != 0)) {
		pass_changed = true;
	}
	bool failover_changed =
		(!src->failover_source && new_failover && new_failover[0]) ||
		(src->failover_source && (!new_failover || strcmp(src->failover_source, new_failover) != 0));

	if (!src->source_name || strcmp(src->source_name, new_name) != 0 || pass_changed || failover_changed ||
	    src->failover_timeout_ms != new_fo_timeout || src->auto_revert != new_auto_revert ||
	    src->auto_revert_hold_ms != new_revert_hold || src->bandwidth != new_bw || src->color_format != new_cf ||
	    src->buffer_mode != new_bm || src->behavior != new_bh || src->transport != new_tr ||
	    src->clear_on_disconnect != new_clear) {
		need_restart = true;
	}
	src->color_range = new_cr;
	src->color_space = new_cs;

	bool disc_groups_changed =
		(!src->discovery_groups && new_disc_groups && new_disc_groups[0]) ||
		(src->discovery_groups && (!new_disc_groups || strcmp(src->discovery_groups, new_disc_groups) != 0));
	bfree(src->discovery_groups);
	src->discovery_groups = bstrdup(new_disc_groups ? new_disc_groups : "");

	if (src->ptz_pan != new_ptz_pan || src->ptz_tilt != new_ptz_tilt || src->ptz_zoom != new_ptz_zoom ||
	    src->ptz_focus != new_ptz_focus) {
		src->ptz_pan = new_ptz_pan;
		src->ptz_tilt = new_ptz_tilt;
		src->ptz_zoom = new_ptz_zoom;
		src->ptz_focus = new_ptz_focus;
		src->ptz_pending = true;
	}
	bfree(src->record_filename);
	src->record_filename = bstrdup(new_rec_file ? new_rec_file : "");

	src->cloud_tally_enabled = new_cloud_enabled;
	bfree(src->cloud_tally_url);
	src->cloud_tally_url = bstrdup(new_cloud_url ? new_cloud_url : "");
	bfree(src->cloud_room);
	src->cloud_room = bstrdup(new_cloud_room ? new_cloud_room : "");
	bfree(src->cloud_key);
	src->cloud_key = bstrdup(new_cloud_key ? new_cloud_key : "");
	src->cloud_slot = new_cloud_slot > 0 ? new_cloud_slot : 1;
	pthread_mutex_unlock(&src->cfg_lock);

	if (disc_groups_changed && g_disc_lock_init) {
		pthread_mutex_lock(&g_disc_lock);
		snprintf(g_disc_groups, sizeof(g_disc_groups), "%s", new_disc_groups ? new_disc_groups : "");
		g_disc_groups_dirty = true;
		pthread_mutex_unlock(&g_disc_lock);
		if (g_disc_wake)
			os_event_signal(g_disc_wake);
	}

	if (need_restart) {
		openavx_source_stop(src);

		pthread_mutex_lock(&src->cfg_lock);
		bfree(src->source_name);
		src->source_name = bstrdup(new_name);
		bfree(src->passphrase);
		src->passphrase = bstrdup(new_pass ? new_pass : "");
		bfree(src->failover_source);
		src->failover_source = bstrdup(new_failover ? new_failover : "");
		src->failover_timeout_ms = new_fo_timeout > 0 ? new_fo_timeout : 2000;
		src->auto_revert = new_auto_revert;
		src->auto_revert_hold_ms = new_revert_hold > 0 ? new_revert_hold : 3000;
		src->bandwidth = new_bw;
		src->color_format = new_cf;
		src->color_range = new_cr;
		src->color_space = new_cs;
		src->buffer_mode = new_bm;
		src->behavior = new_bh;
		src->transport = new_tr;
		src->clear_on_disconnect = new_clear;
		pthread_mutex_unlock(&src->cfg_lock);

		clear_obs_output(src);
		openavx_source_start(src);
	}

	update_tally_from_obs(src);
}

static void *openavx_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct openavx_source *src = (struct openavx_source *)bzalloc(sizeof(struct openavx_source));
	if (!src)
		return NULL;

	src->source = source;
	if (pthread_mutex_init(&src->cfg_lock, NULL) != 0) {
		bfree(src);
		return NULL;
	}
	pthread_mutex_init(&src->telem_lock, NULL);
	src->telem_status[0] = '\0';

	src->ctx = oavx_context_create();
	if (!src->ctx) {
		obs_log(LOG_ERROR, "[source] failed to create OpenAVX context");
		pthread_mutex_destroy(&src->telem_lock);
		pthread_mutex_destroy(&src->cfg_lock);
		bfree(src);
		return NULL;
	}

	os_event_init(&src->cloud_wake, OS_EVENT_TYPE_AUTO);
	os_atomic_store_long(&src->cloud_target_tally, -1);
	src->cloud_last_sent_tally = -1;
	os_atomic_store_bool(&src->cloud_running, true);
	if (pthread_create(&src->cloud_thread, NULL, openavx_cloud_tally_worker, src) == 0) {
		src->cloud_thread_started = true;
	} else {
		src->cloud_thread_started = false;
		obs_log(LOG_WARNING, "[source] failed to start cloud tally worker thread");
	}

	register_active_source(src);

	os_atomic_store_bool(&src->obs_hidden, false);
	openavx_source_update(src, settings);
	return src;
}

static void openavx_source_destroy(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	if (!src)
		return;

	unregister_active_source(src);

	openavx_source_stop(src);

	if (src->cloud_thread_started) {
		os_atomic_store_bool(&src->cloud_running, false);
		if (src->cloud_wake)
			os_event_signal(src->cloud_wake);
		pthread_join(src->cloud_thread, NULL);
		src->cloud_thread_started = false;
	}
	if (src->cloud_wake) {
		os_event_destroy(src->cloud_wake);
		src->cloud_wake = NULL;
	}
	bfree(src->cloud_tally_url);
	bfree(src->cloud_room);
	bfree(src->cloud_key);

	if (src->ctx) {
		oavx_context_destroy(src->ctx);
		src->ctx = NULL;
	}

	bfree(src->scratch_buf);
	bfree(src->source_name);
	bfree(src->passphrase);
	bfree(src->failover_source);
	bfree(src->discovery_groups);
	bfree(src->record_filename);
	pthread_mutex_destroy(&src->telem_lock);
	pthread_mutex_destroy(&src->cfg_lock);
	bfree(src);
}

static void openavx_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "source_name", "");
	obs_data_set_default_string(settings, "passphrase", "");
	obs_data_set_default_int(settings, "bandwidth", OAVX_BANDWIDTH_HIGHEST);
	obs_data_set_default_int(settings, "color_format", OAVX_COLOR_FORMAT_UYVY_BGRA);
	obs_data_set_default_int(settings, "color_range", OPENAVX_COLOR_RANGE_AUTO);
	obs_data_set_default_int(settings, "color_space", OPENAVX_COLOR_SPACE_AUTO);
	obs_data_set_default_int(settings, "buffer_mode", OPENAVX_BUFFER_LOW_LATENCY);
	obs_data_set_default_int(settings, "behavior", OPENAVX_BEHAVIOR_KEEP_RUNNING);
	obs_data_set_default_int(settings, "transport", OAVX_TRANSPORT_TCP);
	obs_data_set_default_bool(settings, "clear_on_disconnect", true);
	obs_data_set_default_string(settings, "failover_source", "");
	obs_data_set_default_int(settings, "failover_timeout_ms", 2000);
	obs_data_set_default_bool(settings, "auto_revert", false);
	obs_data_set_default_int(settings, "auto_revert_hold_ms", 3000);
	obs_data_set_default_string(settings, "discovery_groups", "");
	obs_data_set_default_double(settings, "ptz_pan", 0.0);
	obs_data_set_default_double(settings, "ptz_tilt", 0.0);
	obs_data_set_default_double(settings, "ptz_zoom", 0.0);
	obs_data_set_default_double(settings, "ptz_focus", 0.0);
	obs_data_set_default_string(settings, "record_filename", "");

	obs_data_set_default_int(settings, "ui_section", 0);
	obs_data_set_default_bool(settings, "cloud_tally_enabled", true);
	obs_data_set_default_string(settings, "cloud_tally_url", "http://localhost:3001/api/v1/relay/tally");
	obs_data_set_default_string(settings, "cloud_room", "");
	obs_data_set_default_string(settings, "cloud_key", "");
	obs_data_set_default_int(settings, "cloud_slot", 1);
}

static void add_unique_stream_item(obs_property_t *p_src, const char *label, const char *val)
{
	if (!label || !val || !val[0])
		return;
	size_t count = obs_property_list_item_count(p_src);
	for (size_t i = 0; i < count; ++i) {
		const char *existing_val = obs_property_list_item_string(p_src, i);
		if (existing_val && strcmp(existing_val, val) == 0)
			return;
	}
	obs_property_list_add_string(p_src, label, val);
}

static bool on_refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	if (g_disc_wake) {
		os_event_signal(g_disc_wake);
		os_sleep_ms(120);
	}
	return true;
}

static bool on_ptz_send_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct openavx_source *src = (struct openavx_source *)data;
	if (!src)
		return false;
	pthread_mutex_lock(&src->cfg_lock);
	src->ptz_pending = true;
	pthread_mutex_unlock(&src->cfg_lock);
	return false;
}

static bool on_record_start_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct openavx_source *src = (struct openavx_source *)data;
	if (!src)
		return false;
	pthread_mutex_lock(&src->cfg_lock);
	src->record_cmd_pending = 1;
	pthread_mutex_unlock(&src->cfg_lock);
	return false;
}

static bool on_record_stop_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct openavx_source *src = (struct openavx_source *)data;
	if (!src)
		return false;
	pthread_mutex_lock(&src->cfg_lock);
	src->record_cmd_pending = 2;
	pthread_mutex_unlock(&src->cfg_lock);
	return false;
}

static void apply_ui_section_visibility(obs_properties_t *props, int sec)
{
	if (!props)
		return;
	struct {
		const char *name;
		int id;
	} groups[] = {
		{"grp_general", 0}, {"grp_video_net", 1},       {"grp_failover", 2},
		{"grp_ptz_rec", 3}, {"cloud_tally_enabled", 4},
	};
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
		obs_property_t *p = obs_properties_get(props, groups[i].name);
		if (p)
			obs_property_set_visible(p, sec == 5 || sec == groups[i].id);
	}
}

static bool on_ui_section_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	int sec = settings ? (int)obs_data_get_int(settings, "ui_section") : 0;
	apply_ui_section_visibility(props, sec);
	return true;
}

static obs_properties_t *openavx_source_get_properties(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p_sec = obs_properties_add_list(props, "ui_section", TEXT_UI_SECTION, OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_sec, TEXT_GROUP_GENERAL, 0);
	obs_property_list_add_int(p_sec, TEXT_UI_SEC_VIDEO_NET, 1);
	obs_property_list_add_int(p_sec, TEXT_GROUP_FAILOVER, 2);
	obs_property_list_add_int(p_sec, TEXT_UI_SEC_PTZ_REC, 3);
	obs_property_list_add_int(p_sec, TEXT_CLOUD_TALLY, 4);
	obs_property_list_add_int(p_sec, TEXT_UI_SEC_ALL, 5);
	obs_property_set_modified_callback(p_sec, on_ui_section_modified);

	/* === Group 1: General === */
	obs_properties_t *gen_props = obs_properties_create();
	obs_property_t *p_src = obs_properties_add_list(gen_props, "source_name", TEXT_STREAM_NAME,
							OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

	obs_properties_add_button(gen_props, "refresh_streams", TEXT_REFRESH_STREAMS, on_refresh_clicked);
	obs_properties_add_text(gen_props, "passphrase", TEXT_PASSPHRASE, OBS_TEXT_PASSWORD);

	char stat_buf[300] = {0};
	if (src) {
		pthread_mutex_lock(&src->telem_lock);
		snprintf(stat_buf, sizeof(stat_buf), "%s",
			 src->telem_status[0] ? src->telem_status : "Waiting for stream...");
		pthread_mutex_unlock(&src->telem_lock);
	} else {
		snprintf(stat_buf, sizeof(stat_buf), "Idle");
	}
	obs_property_t *p_stat = obs_properties_add_text(gen_props, "status_info", stat_buf, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p_stat, OBS_TEXT_INFO_NORMAL);

	/* Populate non-blockingly from background discovery watcher */
	if (g_disc_lock_init) {
		pthread_mutex_lock(&g_disc_lock);
		for (size_t i = 0; i < g_disc_count; ++i) {
			if (g_disc_cache[i].address[0] != '\0') {
				char item_str[520];
				snprintf(item_str, sizeof(item_str), "%.255s (%.255s)", g_disc_cache[i].name,
					 g_disc_cache[i].address);
				add_unique_stream_item(p_src, item_str, item_str);
			} else {
				add_unique_stream_item(p_src, g_disc_cache[i].name, g_disc_cache[i].name);
			}
		}
		pthread_mutex_unlock(&g_disc_lock);
	}

	/* Keep currently configured stream visible in dropdown even if temporarily offline */
	if (src) {
		pthread_mutex_lock(&src->cfg_lock);
		if (src->source_name && src->source_name[0] != '\0') {
			add_unique_stream_item(p_src, src->source_name, src->source_name);
		}
		pthread_mutex_unlock(&src->cfg_lock);
	}

	/* Bandwidth selection */
	obs_property_t *p_bw = obs_properties_add_list(gen_props, "bandwidth", TEXT_BANDWIDTH, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_bw, TEXT_BW_HIGHEST, OAVX_BANDWIDTH_HIGHEST);
	obs_property_list_add_int(p_bw, TEXT_BW_LOWEST, OAVX_BANDWIDTH_LOWEST);
	obs_property_list_add_int(p_bw, TEXT_BW_AUDIO_ONLY, OAVX_BANDWIDTH_AUDIO_ONLY);

	/* Inactive / Visibility Behavior */
	obs_property_t *p_bh = obs_properties_add_list(gen_props, "behavior", TEXT_BEHAVIOR, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_bh, TEXT_BH_KEEP_RUNNING, OPENAVX_BEHAVIOR_KEEP_RUNNING);
	obs_property_list_add_int(p_bh, TEXT_BH_STOP_HIDDEN, OPENAVX_BEHAVIOR_STOP_HIDDEN);

	obs_properties_add_bool(gen_props, "clear_on_disconnect", TEXT_CLEAR_DISCONNECT);
	obs_properties_add_group(props, "grp_general", TEXT_GROUP_GENERAL, OBS_GROUP_NORMAL, gen_props);

	/* === Group 2: Video & Network === */
	obs_properties_t *vn_props = obs_properties_create();
	obs_property_t *p_cf = obs_properties_add_list(vn_props, "color_format", TEXT_COLOR_FORMAT, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_cf, TEXT_CF_AUTO, OAVX_COLOR_FORMAT_UYVY_BGRA);
	obs_property_list_add_int(p_cf, TEXT_CF_BGRA, OAVX_COLOR_FORMAT_BGRX_BGRA);
	obs_property_list_add_int(p_cf, TEXT_CF_RGBA, OAVX_COLOR_FORMAT_RGBX_RGBA);
	obs_property_list_add_int(p_cf, TEXT_CF_NATIVE, OAVX_COLOR_FORMAT_BEST);

	obs_property_t *p_cr = obs_properties_add_list(vn_props, "color_range", TEXT_COLOR_RANGE, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_cr, TEXT_CR_AUTO, OPENAVX_COLOR_RANGE_AUTO);
	obs_property_list_add_int(p_cr, TEXT_CR_PARTIAL, OPENAVX_COLOR_RANGE_PARTIAL);
	obs_property_list_add_int(p_cr, TEXT_CR_FULL, OPENAVX_COLOR_RANGE_FULL);

	obs_property_t *p_cs = obs_properties_add_list(vn_props, "color_space", TEXT_COLOR_SPACE, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_cs, TEXT_CS_AUTO, OPENAVX_COLOR_SPACE_AUTO);
	obs_property_list_add_int(p_cs, TEXT_CS_BT709, OPENAVX_COLOR_SPACE_BT709);
	obs_property_list_add_int(p_cs, TEXT_CS_BT601, OPENAVX_COLOR_SPACE_BT601);
	obs_property_list_add_int(p_cs, TEXT_CS_BT2100PQ, OPENAVX_COLOR_SPACE_BT2100_PQ);
	obs_property_list_add_int(p_cs, TEXT_CS_BT2100HLG, OPENAVX_COLOR_SPACE_BT2100_HLG);

	obs_property_t *p_bm = obs_properties_add_list(vn_props, "buffer_mode", TEXT_BUFFER_MODE, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_bm, TEXT_BM_LOW_LATENCY, OPENAVX_BUFFER_LOW_LATENCY);
	obs_property_list_add_int(p_bm, TEXT_BM_FRAMESYNC, OPENAVX_BUFFER_FRAMESYNC);

	obs_properties_add_text(vn_props, "discovery_groups", TEXT_DISCOVERY_GROUPS, OBS_TEXT_DEFAULT);

	obs_property_t *p_tr = obs_properties_add_list(vn_props, "transport", TEXT_TRANSPORT, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_TCP, OAVX_TRANSPORT_TCP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_RUDP, OAVX_TRANSPORT_RUDP);
	obs_property_list_add_int(p_tr, TEXT_TRANSPORT_UDP, OAVX_TRANSPORT_UDP);
	obs_properties_add_group(props, "grp_video_net", TEXT_GROUP_VIDEO_NET, OBS_GROUP_NORMAL, vn_props);

	/* === Group 3: Failover === */
	obs_properties_t *fo_props = obs_properties_create();
	obs_property_t *p_fo = obs_properties_add_list(fo_props, "failover_source", TEXT_FAILOVER_SOURCE,
						       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p_fo, "", "");
	if (g_disc_lock_init) {
		pthread_mutex_lock(&g_disc_lock);
		for (size_t i = 0; i < g_disc_count; ++i) {
			if (g_disc_cache[i].address[0] != '\0') {
				char item_str[520];
				snprintf(item_str, sizeof(item_str), "%.255s (%.255s)", g_disc_cache[i].name,
					 g_disc_cache[i].address);
				add_unique_stream_item(p_fo, item_str, item_str);
			} else {
				add_unique_stream_item(p_fo, g_disc_cache[i].name, g_disc_cache[i].name);
			}
		}
		pthread_mutex_unlock(&g_disc_lock);
	}
	if (src) {
		pthread_mutex_lock(&src->cfg_lock);
		if (src->failover_source && src->failover_source[0] != '\0') {
			add_unique_stream_item(p_fo, src->failover_source, src->failover_source);
		}
		pthread_mutex_unlock(&src->cfg_lock);
	}
	obs_properties_add_int_slider(fo_props, "failover_timeout_ms", TEXT_FAILOVER_TIMEOUT, 200, 30000, 100);
	obs_properties_add_bool(fo_props, "auto_revert", TEXT_AUTO_REVERT);
	obs_properties_add_int_slider(fo_props, "auto_revert_hold_ms", TEXT_AUTO_REVERT_HOLD, 500, 60000, 500);
	obs_properties_add_group(props, "grp_failover", TEXT_GROUP_FAILOVER, OBS_GROUP_NORMAL, fo_props);

	/* === Group 4: PTZ & Remote Recording === */
	obs_properties_t *ptz_props = obs_properties_create();
	obs_properties_add_float_slider(ptz_props, "ptz_pan", TEXT_PTZ_PAN, -1.0, 1.0, 0.05);
	obs_properties_add_float_slider(ptz_props, "ptz_tilt", TEXT_PTZ_TILT, -1.0, 1.0, 0.05);
	obs_properties_add_float_slider(ptz_props, "ptz_zoom", TEXT_PTZ_ZOOM, 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(ptz_props, "ptz_focus", TEXT_PTZ_FOCUS, 0.0, 1.0, 0.05);
	obs_properties_add_button(ptz_props, "ptz_send", TEXT_PTZ_SEND, on_ptz_send_clicked);
	obs_properties_add_text(ptz_props, "record_filename", TEXT_RECORD_FILENAME, OBS_TEXT_DEFAULT);
	obs_properties_add_button(ptz_props, "record_start", TEXT_RECORD_START, on_record_start_clicked);
	obs_properties_add_button(ptz_props, "record_stop", TEXT_RECORD_STOP, on_record_stop_clicked);
	obs_properties_add_group(props, "grp_ptz_rec", TEXT_GROUP_PTZ_REC, OBS_GROUP_NORMAL, ptz_props);

	/* === Group 5: Cloud Tally (Checkable) === */
	obs_properties_t *cloud_props = obs_properties_create();
	obs_properties_add_text(cloud_props, "cloud_tally_url", TEXT_CLOUD_TALLY_URL, OBS_TEXT_DEFAULT);
	obs_properties_add_text(cloud_props, "cloud_room", TEXT_CLOUD_ROOM, OBS_TEXT_DEFAULT);
	obs_properties_add_text(cloud_props, "cloud_key", TEXT_CLOUD_KEY, OBS_TEXT_PASSWORD);
	obs_properties_add_int(cloud_props, "cloud_slot", TEXT_CLOUD_SLOT, 1, 32, 1);
	obs_property_t *p_cloud = obs_properties_add_group(props, "cloud_tally_enabled", TEXT_CLOUD_TALLY,
							   OBS_GROUP_CHECKABLE, cloud_props);

	/* Concise hover tooltips for non-obvious properties */
	obs_property_set_long_description(p_sec, obs_module_text("OpenAVX.Source.Tip.Section"));
	obs_property_set_long_description(p_bw, obs_module_text("OpenAVX.Source.Tip.Bandwidth"));
	obs_property_set_long_description(p_bh, obs_module_text("OpenAVX.Source.Tip.Behavior"));
	obs_property_set_long_description(p_cf, obs_module_text("OpenAVX.Source.Tip.ColorFormat"));
	obs_property_set_long_description(p_cr, obs_module_text("OpenAVX.Source.Tip.ColorRange"));
	obs_property_set_long_description(p_cs, obs_module_text("OpenAVX.Source.Tip.ColorSpace"));
	obs_property_set_long_description(p_bm, obs_module_text("OpenAVX.Source.Tip.BufferMode"));
	obs_property_set_long_description(obs_properties_get(props, "discovery_groups"),
					  obs_module_text("OpenAVX.Source.Tip.DiscoveryGroups"));
	obs_property_set_long_description(p_tr, obs_module_text("OpenAVX.Source.Tip.Transport"));
	obs_property_set_long_description(p_fo, obs_module_text("OpenAVX.Source.Tip.FailoverSource"));
	obs_property_set_long_description(obs_properties_get(props, "failover_timeout_ms"),
					  obs_module_text("OpenAVX.Source.Tip.FailoverTimeout"));
	obs_property_set_long_description(obs_properties_get(props, "auto_revert"),
					  obs_module_text("OpenAVX.Source.Tip.AutoRevert"));
	obs_property_set_long_description(obs_properties_get(props, "auto_revert_hold_ms"),
					  obs_module_text("OpenAVX.Source.Tip.AutoRevertHold"));
	obs_property_set_long_description(p_cloud, obs_module_text("OpenAVX.Source.Tip.CloudTally"));

	/* Always open Properties on General (0) instead of a previously saved tab */
	if (src && src->source) {
		obs_data_t *s = obs_source_get_settings(src->source);
		if (s) {
			obs_data_set_int(s, "ui_section", 0);
			obs_data_release(s);
		}
	}
	apply_ui_section_visibility(props, 0);

	return props;
}

static uint32_t openavx_source_get_width(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	return src ? (uint32_t)os_atomic_load_long(&src->width) : 0;
}

static uint32_t openavx_source_get_height(void *data)
{
	struct openavx_source *src = (struct openavx_source *)data;
	return src ? (uint32_t)os_atomic_load_long(&src->height) : 0;
}

struct obs_source_info openavx_source_info = {
	.id = "openavx_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = openavx_source_get_name,
	.create = openavx_source_create,
	.destroy = openavx_source_destroy,
	.update = openavx_source_update,
	.activate = openavx_source_activate,
	.deactivate = openavx_source_deactivate,
	.show = openavx_source_show,
	.hide = openavx_source_hide,
	.get_defaults = openavx_source_get_defaults,
	.get_properties = openavx_source_get_properties,
	.get_width = openavx_source_get_width,
	.get_height = openavx_source_get_height,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};
