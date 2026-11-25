/*
OpenAVX for OBS Studio - Frontend Output Controller Header
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openavx_output_config {
	bool main_enabled;
	char main_name[256];
	char main_groups[256];

	bool preview_enabled;
	char preview_name[256];
	char preview_groups[256];
	bool preview_include_audio;

	int video_compression;     /* 0 = Uncompressed, 1 = SpeedHQ, 2 = H.264, 3 = HEVC, 4 = AV1 */
	int speedhq_quality;       /* 20..98 */
	int bitrate_kbps;          /* 1000..50000 */
	int keyframe_interval;     /* 15..300 */
	char passphrase[256];      /* Optional E2EE passphrase */
	char bind_ip[128];         /* Optional local bind IP or NIC interface */
	char failover_target[256]; /* Optional downstream failover stream/endpoint */
	bool web_preview_enabled;
	int web_preview_port;
	bool rtsp_enabled;
	int rtsp_port;

	int color_format;      /* oavx_color_format_e */
	int audio_compression; /* oavx_audio_compression_mode_e */
	int transport;         /* oavx_transport_e */
};

struct openavx_output_status {
	bool frontend_ready;

	bool main_active;
	uint64_t main_total_bytes;
	int main_total_frames;
	int main_dropped_frames;
	float main_congestion;
	bool main_tally_program;
	bool main_tally_preview;
	int main_clients;
	char main_web_url[256];
	uint16_t main_rtsp_port;
	char main_last_error[256];

	bool preview_active;
	uint64_t preview_total_bytes;
	int preview_total_frames;
	int preview_dropped_frames;
	float preview_congestion;
	bool preview_tally_program;
	bool preview_tally_preview;
	int preview_clients;
	char preview_last_error[256];
};

void openavx_output_controller_init(void);
void openavx_output_controller_shutdown(void);

void openavx_output_config_set_defaults(struct openavx_output_config *cfg);
bool openavx_output_controller_get_config(struct openavx_output_config *out_cfg);
bool openavx_output_controller_apply_config(const struct openavx_output_config *cfg, bool force_restart);
void openavx_output_controller_get_status(struct openavx_output_status *out_status);

#ifdef __cplusplus
}
#endif
