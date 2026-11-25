/*
OpenAVX for OBS Studio - Source Plugin Header
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stddef.h>

extern struct obs_source_info openavx_source_info;

void openavx_source_global_init(void);
void openavx_source_global_shutdown(void);

bool openavx_source_resolve_endpoint(const char *input, char *out_name, size_t name_sz, char *out_addr, size_t addr_sz);
