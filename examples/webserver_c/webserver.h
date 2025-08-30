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

#define LINK_SPEED 1000000000
#define ETHER_MTU 1500

/* http header templates */
#define HTTP_HEADER_PREFIX "HTTP/1.0 200 OK\r\nContent-Type: "
#define HTTP_HEADER_SUFFIX \
  "\r\nCache-Control: max-age=3600\r\nConnection: close\r\nContent-Length: "
#define HTTP_HEADER_END "\r\n\r\n"

/* request states */
typedef enum {
    REQUEST_STATE_IDLE,
    REQUEST_STATE_READING_HEADER,
    REQUEST_STATE_PARSING,
    REQUEST_STATE_OPENING_FILE,
    REQUEST_STATE_READING_FILE,
    REQUEST_STATE_SENDING_RESPONSE,
    REQUEST_STATE_CLOSING
} request_state_t;

/* http request structure */
typedef struct http_request {
    struct tcp_pcb *pcb;
    request_state_t state;

    char method[16];
    char path[MAX_PATH_LENGTH];
    char version[16];

    uint64_t fs_request_id;
    uint64_t file_fd;
    uint64_t file_size;
    uint64_t file_offset;

    ptrdiff_t path_buffer;
    ptrdiff_t read_buffer;
    ptrdiff_t stat_buffer;

    bool headers_sent;
    bool file_open;
    bool in_use;
    bool is_head_request;
    
    int outstanding_operations;
    bool connection_closed;
    bool fs_operation_in_flight;

    char header_buffer[512];
    size_t header_len;
    size_t header_parsed;
    
    char response_headers[MAX_RESPONSE_HEADER_SIZE];
    char full_path[MAX_PATH_LENGTH];
} http_request_t;

/* buffer management functions */
ptrdiff_t fs_buffer_allocate(void);
void fs_buffer_free(ptrdiff_t buffer);
