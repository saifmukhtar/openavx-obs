/*
OpenAVX for OBS Studio - Output Plugin Header
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-module.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct obs_output_info openavx_output_info;
extern struct obs_output_info openavx_video_output_info;

uint32_t openavx_output_get_tally_flags(obs_output_t *output);
int openavx_output_get_connection_count(obs_output_t *output);
bool openavx_output_get_web_preview_url(obs_output_t *output, char *out_url, size_t max_len);
uint16_t openavx_output_get_rtsp_port(obs_output_t *output);

#ifdef __cplusplus
}
#endif
