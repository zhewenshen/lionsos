/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <lwip/tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* configuration */
#define WEB_ROOT_DIR "www"

#define LWIP_TICK_MS 100
#define HTTP_PORT 80
#define MAX_CONCURRENT_REQUESTS 128
#define FILE_READ_BUFFER_SIZE 0x8000
#define MAX_PATH_LENGTH 4096
#define MAX_RESPONSE_HEADER_SIZE 512

#define KEEPALIVE_TIMEOUT_MS 5000
#define KEEPALIVE_MAX_REQUESTS 256

#define LINK_SPEED 1000000000
#define ETHER_MTU 1500

/* request states */
typedef enum {
    REQUEST_STATE_IDLE,
    REQUEST_STATE_READING_HEADER,
    REQUEST_STATE_PARSING,
    REQUEST_STATE_OPENING_FILE,
    REQUEST_STATE_STAT_FILE,
    REQUEST_STATE_READING_FILE,
    REQUEST_STATE_SENDING_RESPONSE,
    REQUEST_STATE_CLOSING
} request_state_t;

typedef struct http_connection {
    struct tcp_pcb *pcb;
    bool keep_alive;
    int requests_served;
    bool in_use;
    struct http_request *current_request;
    uint64_t idle_since;

    char header_buffer[2048];
    size_t header_len;
    bool header_too_large;
    bool has_pipelined_data;
} http_connection_t;

typedef struct http_request {
    request_state_t state;
    http_connection_t *connection;

    char method[16];
    char path[MAX_PATH_LENGTH];
    char version[16];
    uint64_t if_modified_since;

    uint64_t fs_request_id;
    uint64_t file_fd;
    uint64_t file_size;
    uint64_t file_offset;
    uint64_t file_mtime;

    ptrdiff_t path_buffer;
    ptrdiff_t read_buffer;
    ptrdiff_t stat_buffer;

    bool headers_sent;
    bool file_open;
    bool is_head_request;

    int outstanding_operations;
    bool fs_operation_in_flight;

    char response_headers[MAX_RESPONSE_HEADER_SIZE];
    char full_path[MAX_PATH_LENGTH];
} http_request_t;

/* buffer management functions */
ptrdiff_t fs_buffer_allocate(void);
void fs_buffer_free(ptrdiff_t buffer);
