/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

int normalize_path(char *path, size_t len);

void format_http_date_from_unix(char *buffer, size_t buffer_size, uint64_t unix_timestamp);
uint64_t parse_http_date(const char *date_str);
