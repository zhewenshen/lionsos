/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "webserver.h"

#include <assert.h>
#include <lions/fs/config.h>
#include <lions/fs/protocol.h>
#include <lwip/dhcp.h>
#include <lwip/err.h>
#include <lwip/init.h>
#include <lwip/ip.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/snmp.h>
#include <lwip/stats.h>
#include <lwip/sys.h>
#include <lwip/tcp.h>
#include <lwip/timeouts.h>
#include <microkit.h>
#include <netif/etharp.h>
#include <sddf/network/config.h>
#include <sddf/network/constants.h>
#include <sddf/network/lib_sddf_lwip.h>
#include <sddf/network/queue.h>
#include <sddf/network/util.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>
#include <sddf/util/cache.h>
#include <sddf/util/printf.h>
#include <sddf/util/util.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "picohttpparser.h"
#include "utils.h"
#include <sddf/timer/client.h>

__attribute__((__section__(".serial_client_config"))) serial_client_config_t serial_config;
__attribute__((__section__(".timer_client_config"))) timer_client_config_t timer_config;
__attribute__((__section__(".net_client_config"))) net_client_config_t net_config;
__attribute__((__section__(".fs_client_config"))) fs_client_config_t fs_config;
__attribute__((__section__(".lib_sddf_lwip_config"))) lib_sddf_lwip_config_t lib_sddf_lwip_config;

serial_queue_handle_t serial_tx_queue_handle;
net_queue_handle_t net_rx_queue;
net_queue_handle_t net_tx_queue;

fs_queue_t *fs_command_queue;
fs_queue_t *fs_completion_queue;
char *fs_share;

static bool net_enabled = false;

static http_connection_t connections[MAX_CONCURRENT_REQUESTS];
static http_request_t requests[MAX_CONCURRENT_REQUESTS];
static bool fs_initialized = false;

static uint64_t id_counter = 1;

static uint64_t connection_bitmap[2] = { 0 };
static int connection_next_hint = 0;
static uint64_t request_bitmap[2] = { 0 };
static int request_next_hint = 0;

#define FS_BUFFER_COUNT (FS_QUEUE_CAPACITY * 2)
#define FS_BITMAP_WORDS ((FS_BUFFER_COUNT + 63) / 64)

#define FS_OP_SAFE(req)                                                        \
  (req->connection && req->connection->in_use && !req->fs_operation_in_flight)

#define HTTP_ERROR_AND_CLOSE(req, code, msg)                                   \
  do {                                                                         \
    if (req->connection && req->connection->pcb)                               \
      send_http_error(req->connection->pcb, code, msg,                         \
                     (req->version[0] ? req->version : "1.0"));                \
    request_close_connection(req);                                             \
  } while (0)

#define BITMAP_WORD(idx) ((idx) / 64)
#define BITMAP_BIT(idx) ((idx) % 64)
#define BITMAP_MASK(idx) (1ULL << BITMAP_BIT(idx))

#define SUBMIT_FS_CMD(cmd)                                                     \
  do {                                                                         \
    fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;                        \
    fs_queue_publish_production(fs_command_queue, 1);                          \
    microkit_notify(fs_config.server.id);                                      \
  } while (0)

static uint64_t fs_buffer_bitmap[FS_BITMAP_WORDS];
static int fs_buffer_next_hint = 0;

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void http_error(void *arg, err_t err);
static err_t http_poll_timeout(void *arg, struct tcp_pcb *pcb);
static void request_start_operation(http_request_t *req);
static void process_pipelined_requests(http_connection_t *conn);
static void handle_stat_completion(http_request_t *req, fs_cmpl_t completion);
static void handle_open_completion(http_request_t *req, fs_cmpl_t completion);
static void handle_read_completion(http_request_t *req, fs_cmpl_t completion);

static http_connection_t *connection_alloc(void);
static void connection_free(http_connection_t *conn);
static http_request_t *request_alloc(void);
static void request_free(http_request_t *req);

static int bitmap_alloc(uint64_t *bitmap, int max_count, int *hint)
{
    int start = *hint;
    for (int i = 0; i < max_count; i++) {
        int idx = (start + i) % max_count;
        uint64_t mask = BITMAP_MASK(idx);

        if (!(bitmap[BITMAP_WORD(idx)] & mask)) {
            bitmap[BITMAP_WORD(idx)] |= mask;
            *hint = (idx + 1) % max_count;
            return idx;
        }
    }
    return -1;
}

static void bitmap_free(uint64_t *bitmap, int idx, int max_count, int *hint)
{
    if (idx >= 0 && idx < max_count) {
        bitmap[BITMAP_WORD(idx)] &= ~BITMAP_MASK(idx);
        *hint = idx;
    }
}

static void tcp_cleanup_callbacks(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
}

static err_t http_poll_timeout(void *arg, struct tcp_pcb *pcb)
{
    http_connection_t *conn = (http_connection_t *)arg;

    if (!conn || !conn->in_use) {
        return ERR_OK;
    }

    if (!conn->current_request && conn->idle_since > 0) {
        uint64_t current_time = sddf_timer_time_now(timer_config.driver_id);
        uint64_t idle_duration_ns = current_time - conn->idle_since;
        uint64_t idle_duration_ms = idle_duration_ns / 1000000ULL;
        uint64_t timeout_ns = KEEPALIVE_TIMEOUT_MS * 1000000ULL;

        sddf_dprintf("Poll check: pcb=%p (port %d->%d), idle=%lums/%dms, requests_served=%d\n",
                     (void*)pcb, pcb->remote_port, pcb->local_port, idle_duration_ms, KEEPALIVE_TIMEOUT_MS, conn->requests_served);

        if (idle_duration_ns >= timeout_ns) {
            sddf_dprintf("Poll timeout: CLOSING idle connection pcb=%p (port %d->%d), idle=%lums >= %dms\n",
                         (void*)pcb, pcb->remote_port, pcb->local_port, idle_duration_ms, KEEPALIVE_TIMEOUT_MS);
            connection_free(conn);
            return ERR_ABRT;
        } else {
            sddf_dprintf("Poll timeout: KEEPING connection pcb=%p (port %d->%d), idle=%lums < %dms\n",
                         (void*)pcb, pcb->remote_port, pcb->local_port, idle_duration_ms, KEEPALIVE_TIMEOUT_MS);
        }
    } else if (conn->current_request) {
        sddf_dprintf("Poll check: pcb=%p (port %d->%d) is ACTIVE (processing request), skipping timeout\n",
                     (void*)pcb, pcb->remote_port, pcb->local_port);
    }

    return ERR_OK;
}

static bool allocate_request_buffers(http_request_t *req);
static void free_request_buffers(http_request_t *req);

static void send_file_read_command(http_request_t *req, uint64_t offset, size_t size)
{
    request_start_operation(req);
    fs_cmd_t cmd = { .type = FS_CMD_FILE_READ,
                     .id = req->fs_request_id,
                     .params.file_read = {
                         .fd = req->file_fd, .offset = offset, .buf = { .offset = req->read_buffer, .size = size } } };
    SUBMIT_FS_CMD(cmd);
}

static void finish_response_and_close(http_request_t *req)
{
    http_connection_t *conn = req->connection;
    
    if (conn->pcb) {
        tcp_output(conn->pcb);
    }

    if (conn->keep_alive && conn->requests_served < KEEPALIVE_MAX_REQUESTS) {
        conn->requests_served++;
        conn->idle_since = sddf_timer_time_now(timer_config.driver_id);
        uint64_t idle_timestamp_ms = conn->idle_since / 1000000ULL;

        sddf_dprintf("Keep-alive: request complete on pcb=%p (port %d->%d), NOW IDLE (timestamp=%lums), requests_served=%d\n",
                   (void*)conn->pcb, conn->pcb->remote_port, conn->pcb->local_port, idle_timestamp_ms, conn->requests_served);

        request_free(req);
        conn->current_request = NULL;

        if (conn->has_pipelined_data) {
            process_pipelined_requests(conn);
        }
    } else {
        sddf_dprintf("Closing connection: keep_alive=%d, requests_served=%d\n", 
                   conn->keep_alive, conn->requests_served);
        connection_free(conn);
    }
}

static uint64_t request_id_alloc(void)
{
    uint64_t id = id_counter++;
    if (id == 0)
        id = id_counter++; /* skip 0 after wrap */
    return id;
}

static void request_start_operation(http_request_t *req)
{
    if (req) {
        req->outstanding_operations++;
        req->fs_operation_in_flight = true;
    }
}

static void request_complete_operation(http_request_t *req)
{
    if (req) {
        req->outstanding_operations--;
        req->fs_operation_in_flight = false;
    }
}

static bool allocate_request_buffers(http_request_t *req)
{
    req->path_buffer = fs_buffer_allocate();
    req->read_buffer = fs_buffer_allocate();
    req->stat_buffer = fs_buffer_allocate();
    
    if (req->path_buffer == -1 || req->read_buffer == -1 || req->stat_buffer == -1) {
        free_request_buffers(req);
        return false;
    }
    
    return true;
}

static void free_request_buffers(http_request_t *req)
{
    if (req->path_buffer != -1) {
        fs_buffer_free(req->path_buffer);
        req->path_buffer = -1;
    }
    if (req->read_buffer != -1) {
        fs_buffer_free(req->read_buffer);
        req->read_buffer = -1;
    }
    if (req->stat_buffer != -1) {
        fs_buffer_free(req->stat_buffer);
        req->stat_buffer = -1;
    }
}

static const char *content_type_from_extension(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcmp(ext, ".json") == 0)
        return "application/json";
    if (strcmp(ext, ".txt") == 0)
        return "text/plain";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcmp(ext, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(ext, ".pdf") == 0)
        return "application/pdf";

    return "application/octet-stream";
}

static http_connection_t *connection_alloc(void)
{
    int idx = bitmap_alloc(connection_bitmap, MAX_CONCURRENT_REQUESTS, &connection_next_hint);
    if (idx == -1) {
        return NULL;
    }

    memset(&connections[idx], 0, sizeof(http_connection_t));
    connections[idx].in_use = true;
    connections[idx].keep_alive = false;
    connections[idx].requests_served = 0;
    connections[idx].current_request = NULL;
    connections[idx].header_too_large = false;
    connections[idx].has_pipelined_data = false;
    connections[idx].idle_since = 0;
    return &connections[idx];
}

static void connection_free(http_connection_t *conn)
{
    if (!conn || !conn->in_use) {
        sddf_dprintf("connection_free: called on invalid/unused connection %p\n", (void*)conn);
        return;
    }

    int idx = conn - connections;
    sddf_dprintf("connection_free: freeing connection #%d, pcb=%p, in_use=%d\n", 
                 idx, (void*)conn->pcb, conn->in_use);


    if (conn->current_request) {
        sddf_dprintf("connection_free: freeing current request for connection #%d\n", idx);
        request_free(conn->current_request);
        conn->current_request = NULL;
    }

    if (conn->pcb) {
        sddf_dprintf("connection_free: closing TCP pcb=%p for connection #%d\n", 
                     (void*)conn->pcb, idx);
        tcp_cleanup_callbacks(conn->pcb);
        tcp_close(conn->pcb);
        conn->pcb = NULL;
    }

    bitmap_free(connection_bitmap, idx, MAX_CONCURRENT_REQUESTS, &connection_next_hint);
    conn->in_use = false;
    
    sddf_dprintf("connection_free: connection #%d freed, in_use now=%d\n", idx, conn->in_use);
}

static http_request_t *request_alloc(void)
{
    int idx = bitmap_alloc(request_bitmap, MAX_CONCURRENT_REQUESTS, &request_next_hint);
    if (idx == -1)
        return NULL;

    memset(&requests[idx], 0, sizeof(http_request_t));
    requests[idx].state = REQUEST_STATE_IDLE;
    requests[idx].file_fd = UINT64_MAX;
    requests[idx].outstanding_operations = 0;
    requests[idx].fs_operation_in_flight = false;
    requests[idx].path_buffer = -1;
    requests[idx].read_buffer = -1;
    requests[idx].stat_buffer = -1;
    return &requests[idx];
}

static void request_free(http_request_t *req)
{
    if (!req)
        return;

    if (req->file_open && req->file_fd != UINT64_MAX && req->fs_request_id > 0) {
        sddf_dprintf("Closing file fd=%lu during request cleanup\n", req->file_fd);
        fs_cmd_t cmd = { .type = FS_CMD_FILE_CLOSE, .id = req->fs_request_id, .params.file_close.fd = req->file_fd };
        SUBMIT_FS_CMD(cmd);
    }

    free_request_buffers(req);

    int idx = req - requests;
    bitmap_free(request_bitmap, idx, MAX_CONCURRENT_REQUESTS, &request_next_hint);
}

static void request_close_connection(http_request_t *req)
{
    if (!req)
        return;

    if (req->connection) {
        connection_free(req->connection);
        req->connection = NULL;
    }
}


static const char *get_cache_control(const char *content_type)
{
    if (strcmp(content_type, "text/html") == 0 || strcmp(content_type, "text/css") == 0
        || strcmp(content_type, "application/javascript") == 0) {
        return "max-age=600";
    }
    return "max-age=31536000";
}

static int build_http_response(char *buffer, size_t buffer_size, int status_code, const char *status_text,
                              const char *content_type, uint64_t content_length, const char *extra_headers,
                              const char *body, bool keep_alive, const char *http_version)
{
    const char *connection = keep_alive ? "keep-alive" : "close";
    int len = 0;

    len = snprintf(buffer, buffer_size,
                   "HTTP/%s %d %s\r\n"
                   "Date: Mon, 01 Jan 2024 00:00:00 GMT\r\n"
                   "Server: LionsOS/1.0\r\n",
                   http_version, status_code, status_text);
    
    if (len < 0 || len >= buffer_size)
        return -1;
    
    if (extra_headers) {
        int extra_len = snprintf(buffer + len, buffer_size - len, "%s", extra_headers);
        if (extra_len < 0 || len + extra_len >= buffer_size)
            return -1;
        len += extra_len;
    }
    
    if (content_type) {
        int ct_len = snprintf(buffer + len, buffer_size - len, "Content-Type: %s\r\n", content_type);
        if (ct_len < 0 || len + ct_len >= buffer_size)
            return -1;
        len += ct_len;
    }
    
    if (content_length > 0) {
        int cl_len = snprintf(buffer + len, buffer_size - len, "Content-Length: %lu\r\n", content_length);
        if (cl_len < 0 || len + cl_len >= buffer_size)
            return -1;
        len += cl_len;
    }
    
    int conn_len = snprintf(buffer + len, buffer_size - len, "Connection: %s\r\n\r\n", connection);
    if (conn_len < 0 || len + conn_len >= buffer_size)
        return -1;
    len += conn_len;
    
    if (body) {
        size_t body_len = strlen(body);
        if (len + body_len >= buffer_size)
            return -1;
        memcpy(buffer + len, body, body_len);
        len += body_len;
    }
    
    return len;
}

static int build_http_headers(char *buffer, size_t buffer_size, const char *content_type, uint64_t content_length,
                              uint64_t mtime, bool keep_alive, const char *http_version)
{
    char last_modified_buffer[64];
    format_http_date_from_unix(last_modified_buffer, sizeof(last_modified_buffer), mtime);

    const char *cache_control = get_cache_control(content_type);
    const char *connection = keep_alive ? "keep-alive" : "close";

    sddf_dprintf("Building HTTP headers with Connection: %s\n", connection);

    int len = snprintf(buffer, buffer_size,
                       "HTTP/%s 200 OK\r\n"
                       "Date: Mon, 01 Jan 2024 00:00:00 GMT\r\n"
                       "Server: LionsOS/1.0\r\n"
                       "Allow: GET, HEAD\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %lu\r\n"
                       "Last-Modified: %s\r\n"
                       "Cache-Control: %s\r\n"
                       "Connection: %s\r\n"
                       "\r\n",
                       http_version, content_type, content_length, last_modified_buffer, cache_control, connection);

    if (len < 0 || len >= buffer_size)
        return -1;

    return len;
}

static void send_http_error(struct tcp_pcb *pcb, int code, const char *status, const char *http_version)
{
    static char response[256];
    static char body[64];
    const char *extra_headers = NULL;
    int len;

    if (!http_version || http_version[0] == '\0') {
        http_version = "1.0";
    }

    if (code == 405) {
        extra_headers = "Allow: GET, HEAD\r\n";
        strcpy(body, "405 Method Not Allowed\n");
    } else {
        snprintf(body, sizeof(body), "%d %s\n", code, status);
    }

    len = build_http_response(response, sizeof(response), code, status, "text/plain",
                             strlen(body), extra_headers, body, false, http_version);

    if (len > 0) {
        tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    }
}

static void parse_http_request(http_request_t *req)
{
    http_connection_t *conn = req->connection;
    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[16];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int pret = phr_parse_request(conn->header_buffer, conn->header_len, &method, &method_len, &path, &path_len,
                                 &minor_version, headers, &num_headers, 0);

    if (pret == -1) {
        HTTP_ERROR_AND_CLOSE(req, 400, "Bad Request");
        return;
    } else if (pret == -2) {
        return;
    }

    char *first_line_end = strstr(conn->header_buffer, "\r\n");
    if (first_line_end) {
        size_t request_line_len = first_line_end - conn->header_buffer;
        if (request_line_len > 2048) {
            HTTP_ERROR_AND_CLOSE(req, 414, "URI Too Long");
            return;
        }
    }

    if (method_len >= sizeof(req->method)) {
        HTTP_ERROR_AND_CLOSE(req, 400, "Bad Request");
        return;
    }
    memcpy(req->method, method, method_len);
    req->method[method_len] = '\0';

    bool is_get = (method_len == 3 && memcmp(method, "GET", 3) == 0);
    bool is_head = (method_len == 4 && memcmp(method, "HEAD", 4) == 0);

    if (!is_get && !is_head) {
        HTTP_ERROR_AND_CLOSE(req, 405, "Method Not Allowed");
        return;
    }

    req->is_head_request = is_head;
    req->if_modified_since = 0;
    conn->keep_alive = (minor_version >= 1);
    bool has_host = false;

    sddf_dprintf("HTTP/%d.%d request parsing, default keep_alive=%d\n", 1, minor_version, conn->keep_alive);

    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 17 && strncasecmp(headers[i].name, "If-Modified-Since", 17) == 0) {
            char date_buffer[128];
            size_t date_len = MIN(headers[i].value_len, sizeof(date_buffer) - 1);
            memcpy(date_buffer, headers[i].value, date_len);
            date_buffer[date_len] = '\0';
            req->if_modified_since = parse_http_date(date_buffer);
        } else if (headers[i].name_len == 10 && strncasecmp(headers[i].name, "Connection", 10) == 0) {
            if (headers[i].value_len >= 5 && strncasecmp(headers[i].value, "close", 5) == 0) {
                conn->keep_alive = false;
                sddf_dprintf("Connection: close header found, keep_alive=false\n");
            } else if (headers[i].value_len >= 10 && strncasecmp(headers[i].value, "keep-alive", 10) == 0) {
                conn->keep_alive = true;
                sddf_dprintf("Connection: keep-alive header found, keep_alive=true\n");
            }
        } else if (headers[i].name_len == 4 && strncasecmp(headers[i].name, "Host", 4) == 0) {
            has_host = true;
        }
    }

    if (minor_version >= 1 && !has_host) {
        snprintf(req->version, sizeof(req->version), "1.%d", minor_version);
        HTTP_ERROR_AND_CLOSE(req, 400, "Bad Request");
        return;
    }

    const char *query = memchr(path, '?', path_len);
    size_t actual_path_len = query ? (size_t)(query - path) : path_len;

    if (actual_path_len == 0 || path[0] != '/') {
        HTTP_ERROR_AND_CLOSE(req, 400, "Bad Request");
        return;
    }

    if (actual_path_len >= sizeof(req->path)) {
        HTTP_ERROR_AND_CLOSE(req, 414, "URI Too Long");
        return;
    }
    memcpy(req->path, path, actual_path_len);
    req->path[actual_path_len] = '\0';

    int norm_result = normalize_path(req->path, actual_path_len);
    if (norm_result < 0) {
        HTTP_ERROR_AND_CLOSE(req, 400, "Bad Request");
        return;
    }

    if (req->path[norm_result - 1] == '/') {
        if (norm_result + 10 >= sizeof(req->path)) {
            HTTP_ERROR_AND_CLOSE(req, 414, "URI Too Long");
            return;
        }
        strcat(req->path, "index.html");
    }

    snprintf(req->version, sizeof(req->version), "1.%d", minor_version);
    req->state = REQUEST_STATE_STAT_FILE;
}

static void handle_stat_completion(http_request_t *req, fs_cmpl_t completion)
{
    if (completion.status == FS_STATUS_SUCCESS) {
        fs_stat_t *stat = (fs_stat_t *)(fs_share + req->stat_buffer);
        req->file_size = stat->size;
        req->file_mtime = stat->mtime;

        if (req->if_modified_since > 0 && req->file_mtime <= req->if_modified_since && req->connection && req->connection->pcb) {
            static char response[256];
            const char *cache_control_header = "Cache-Control: max-age=3600\r\n";

            int len = build_http_response(response, sizeof(response), 304, "Not Modified", NULL, 0,
                                        cache_control_header, NULL, req->connection->keep_alive, req->version);

            if (len > 0) {
                tcp_write(req->connection->pcb, response, len, TCP_WRITE_FLAG_COPY);
            }
            finish_response_and_close(req);
        } else if (FS_OP_SAFE(req)) {
            request_start_operation(req);
            fs_cmd_t cmd = { .type = FS_CMD_FILE_OPEN,
                             .id = req->fs_request_id,
                             .params.file_open = {
                                 .path = { .offset = req->path_buffer, .size = strlen(req->full_path) },
                                 .flags = FS_OPEN_FLAGS_READ_ONLY } };
            SUBMIT_FS_CMD(cmd);
            req->state = REQUEST_STATE_OPENING_FILE;
        } else {
            request_close_connection(req);
        }
    } else {
        HTTP_ERROR_AND_CLOSE(req, 404, "Not Found");
    }
}

static void handle_open_completion(http_request_t *req, fs_cmpl_t completion)
{
    if (completion.status == FS_STATUS_SUCCESS) {
        req->file_fd = completion.data.file_open.fd;
        req->file_open = true;

        if (FS_OP_SAFE(req)) {
            send_file_read_command(req, 0, MIN(FILE_READ_BUFFER_SIZE, req->file_size));
            req->state = REQUEST_STATE_READING_FILE;
        } else {
            request_close_connection(req);
        }
    } else {
        HTTP_ERROR_AND_CLOSE(req, 500, "Internal Server Error");
    }
}

static void handle_read_completion(http_request_t *req, fs_cmpl_t completion)
{
    if (completion.status == FS_STATUS_SUCCESS) {

        size_t bytes_read = completion.data.file_read.len_read;

        if (!req->headers_sent && req->connection && req->connection->pcb) {
            int header_len = build_http_headers(req->response_headers, sizeof(req->response_headers),
                                                content_type_from_extension(req->path), req->file_size,
                                                req->file_mtime, req->connection->keep_alive, req->version);

            if (header_len > 0) {
                tcp_write(req->connection->pcb, req->response_headers, header_len, TCP_WRITE_FLAG_MORE);
                req->headers_sent = true;

                if (req->is_head_request) {
                    finish_response_and_close(req);
                    return;
                }
            }
        }

        if (bytes_read > 0) {
            if (!req->is_head_request && req->connection && req->connection->pcb) {
                /* Use COPY flag for last chunk, MORE for intermediate chunks */
                bool is_last_chunk = (req->file_offset + bytes_read >= req->file_size);
                tcp_write(req->connection->pcb, fs_share + req->read_buffer, bytes_read, 
                         is_last_chunk ? TCP_WRITE_FLAG_COPY : TCP_WRITE_FLAG_MORE);
            }
            req->file_offset += bytes_read;

            if (req->file_offset < req->file_size && FS_OP_SAFE(req)) {
                send_file_read_command(req, req->file_offset, FILE_READ_BUFFER_SIZE);
            } else {
                finish_response_and_close(req);
            }
        } else {
            finish_response_and_close(req);
        }
    } else {
        HTTP_ERROR_AND_CLOSE(req, 500, "Internal Server Error");
    }
}

static void process_file_operations(void)
{
    uint64_t to_consume = fs_queue_length_consumer(fs_completion_queue);

    for (uint64_t i = 0; i < to_consume; i++) {
        fs_cmpl_t completion = fs_queue_idx_filled(fs_completion_queue, i)->cmpl;

        if (completion.id == 0 && !fs_initialized) {
            if (completion.status == FS_STATUS_SUCCESS) {
                fs_initialized = true;
                sddf_dprintf("File system initialized\n");
            } else {
                sddf_dprintf("File system initialization failed\n");
            }
            continue;
        }

        http_request_t *req = NULL;
        for (int j = 0; j < MAX_CONCURRENT_REQUESTS; j++) {
            if (connections[j].in_use && connections[j].current_request && 
                connections[j].current_request->fs_request_id == completion.id) {
                req = connections[j].current_request;
                break;
            }
        }

        if (!req) {
            continue;
        }

        request_complete_operation(req);


        if (!req->connection || !req->connection->in_use) {
            request_free(req);
            continue;
        }

        switch (req->state) {
        case REQUEST_STATE_STAT_FILE:
            handle_stat_completion(req, completion);
            break;

        case REQUEST_STATE_OPENING_FILE:
            handle_open_completion(req, completion);
            break;

        case REQUEST_STATE_READING_FILE:
            handle_read_completion(req, completion);
            break;

        default:
            request_close_connection(req);
            break;
        }
    }

    fs_queue_publish_consumption(fs_completion_queue, to_consume);
}

static void process_pipelined_requests(http_connection_t *conn)
{
    while (conn->in_use && conn->header_len > 0 && !conn->current_request) {
        char *header_end = strstr(conn->header_buffer, "\r\n\r\n");
        if (!header_end) {
            /* No complete request yet */
            break;
        }
        
        sddf_dprintf("Processing pipelined request on pcb=%p (port %d->%d)\n", 
                     (void*)conn->pcb, conn->pcb->remote_port, conn->pcb->local_port);
        
        http_request_t *req = request_alloc();
        if (!req) {
            connection_free(conn);
            return;
        }
        
        req->connection = conn;
        conn->current_request = req;
        conn->idle_since = 0;
        req->state = REQUEST_STATE_PARSING;
        req->fs_request_id = request_id_alloc();

        sddf_dprintf("New request: pcb=%p (port %d->%d) NOW ACTIVE, idle_since cleared\n",
                     (void*)conn->pcb, conn->pcb->remote_port, conn->pcb->local_port);


        if (req->fs_request_id == 0) {
            request_free(req);
            conn->current_request = NULL;
            connection_free(conn);
            return;
        }
        
        parse_http_request(req);
        
        /* Move remaining data to start of buffer */
        size_t consumed = (header_end + 4) - conn->header_buffer;
        size_t remaining = conn->header_len - consumed;
        if (remaining > 0) {
            memmove(conn->header_buffer, header_end + 4, remaining);
            conn->has_pipelined_data = true;
        } else {
            conn->has_pipelined_data = false;
        }
        conn->header_len = remaining;
        conn->header_buffer[conn->header_len] = '\0';
        
        /* Start processing the request */
        if (req->state == REQUEST_STATE_STAT_FILE && FS_OP_SAFE(req)) {
            if (!allocate_request_buffers(req)) {
                connection_free(conn);
                return;
            }
            
            snprintf(req->full_path, sizeof(req->full_path), "%s%s", WEB_ROOT_DIR, req->path);
            size_t path_len = strlen(req->full_path);
            memcpy(fs_share + req->path_buffer, req->full_path, path_len + 1);
            
            request_start_operation(req);
            fs_cmd_t cmd = { .type = FS_CMD_STAT,
                             .id = req->fs_request_id,
                             .params.stat = {
                                 .path = { .offset = req->path_buffer, .size = path_len },
                                 .buf = { .offset = req->stat_buffer, .size = sizeof(fs_stat_t) } } };
            SUBMIT_FS_CMD(cmd);
            
            break;
        } else if (req->state == REQUEST_STATE_STAT_FILE) {
            connection_free(conn);
            return;
        }
    }
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_connection_t *conn = (http_connection_t *)arg;

    if (!conn || !conn->in_use) {
        if (p != NULL) {
            pbuf_free(p);
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    if (p == NULL) {
        connection_free(conn);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return err;
    }

    size_t copy_len = p->tot_len;
    if (conn->header_len + copy_len > sizeof(conn->header_buffer) - 1) {
        copy_len = sizeof(conn->header_buffer) - 1 - conn->header_len;
        if (copy_len == 0) {
            conn->header_too_large = true;
        }
    }

    if (!conn->header_too_large) {
        pbuf_copy_partial(p, conn->header_buffer + conn->header_len, copy_len, 0);
        conn->header_len += copy_len;
        conn->header_buffer[conn->header_len] = '\0';
    }

    if (conn->header_too_large ||
        (conn->header_len >= sizeof(conn->header_buffer) - 1 && !strstr(conn->header_buffer, "\r\n\r\n"))) {
        static char response[256];
        static char body[] = "431 Request Header Fields Too Large\n";

        int len = build_http_response(response, sizeof(response), 431, "Request Header Fields Too Large",
                                    "text/plain", strlen(body), NULL, body, false, "1.0");

        if (len > 0) {
            tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
        }
        connection_free(conn);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    process_pipelined_requests(conn);

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (!newpcb) {
        return ERR_ABRT;
    }

    http_connection_t *conn = connection_alloc();
    if (!conn) {
        tcp_close(newpcb);
        return ERR_MEM;
    }

    conn->pcb = newpcb;
    
    sddf_dprintf("New TCP connection accepted: local_port=%d, remote_port=%d, pcb=%p\n", 
                 newpcb->local_port, newpcb->remote_port, (void*)newpcb);

    tcp_nagle_disable(newpcb);

    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, http_recv);
    tcp_err(newpcb, http_error);
    tcp_poll(newpcb, http_poll_timeout, 10);

    return ERR_OK;
}

static void http_error(void *arg, err_t err)
{
    http_connection_t *conn = (http_connection_t *)arg;
    if (conn && conn->in_use) {
        conn->pcb = NULL;
        connection_free(conn);
    }
}

static void setup_http_server(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) {
        sddf_dprintf("Failed to create TCP PCB\n");
        return;
    }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, 80);
    if (err != ERR_OK) {
        sddf_dprintf("Failed to bind TCP PCB: %d\n", err);
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        sddf_dprintf("Failed to listen on TCP PCB\n");
        return;
    }

    tcp_accept(pcb, http_accept);
    sddf_dprintf("HTTP server listening on port 80\n");
    sddf_dprintf("HTTP server ready - listening on port 80 (sddf_printf test)\n");
}

static void netif_status_callback(char *ip_addr)
{
    sddf_dprintf("DHCP request finished, IP address for netif %s is: %s\n", "webserver_c", ip_addr);
    setup_http_server();
}

static void init_networking(void)
{
    net_queue_init(&net_rx_queue, net_config.rx.free_queue.vaddr, net_config.rx.active_queue.vaddr,
                   net_config.rx.num_buffers);
    net_queue_init(&net_tx_queue, net_config.tx.free_queue.vaddr, net_config.tx.active_queue.vaddr,
                   net_config.tx.num_buffers);
    net_buffers_init(&net_tx_queue, 0);

    sddf_lwip_init(&lib_sddf_lwip_config, &net_config, &timer_config, net_rx_queue, net_tx_queue, NULL, NULL,
                   netif_status_callback, NULL, NULL, NULL);

    sddf_timer_set_timeout(timer_config.driver_id, 100 * NS_IN_MS);

    sddf_lwip_maybe_notify();
}

void init(void)
{
    assert(fs_config_check_magic(&fs_config));

    fs_command_queue = fs_config.server.command_queue.vaddr;
    fs_completion_queue = fs_config.server.completion_queue.vaddr;
    fs_share = fs_config.server.share.vaddr;

    serial_queue_init(&serial_tx_queue_handle, serial_config.tx.queue.vaddr, serial_config.tx.data.size,
                      serial_config.tx.data.vaddr);
    serial_putchar_init(serial_config.tx.id, &serial_tx_queue_handle);
    
    sddf_dprintf("webserver_c: Starting initialization after serial init\n");

    net_enabled = net_config_check_magic(&net_config);
    if (net_enabled) {
        init_networking();
    } else {
    }

    fs_cmd_t cmd = { .type = FS_CMD_INITIALISE, .id = 0 };
    SUBMIT_FS_CMD(cmd);
}

void notified(microkit_channel ch)
{
    if (net_enabled && ch == net_config.rx.id) {
        sddf_lwip_process_rx();
    } else if (net_enabled && ch == net_config.tx.id) {
        /* handled by lib_sddf_lwip */
    } else if (ch == timer_config.driver_id) {
        if (net_enabled) {
            sddf_lwip_process_timeout();
            sddf_timer_set_timeout(timer_config.driver_id, 100 * NS_IN_MS);
        }
    } else if (ch == fs_config.server.id) {
        process_file_operations();
    } else if (ch == serial_config.tx.id) {
        sddf_dprintf("serial TX notification received\n");
    } else {
        sddf_dprintf("unknown channel notification: %u\n", ch);
    }

    if (net_enabled) {
        sddf_lwip_maybe_notify();
    }
}

/* fs buffer management */
ptrdiff_t fs_buffer_allocate(void)
{
    int idx = bitmap_alloc(fs_buffer_bitmap, FS_BUFFER_COUNT, &fs_buffer_next_hint);
    return idx == -1 ? -1 : idx * FILE_READ_BUFFER_SIZE;
}

void fs_buffer_free(ptrdiff_t buffer)
{
    int idx = buffer / FILE_READ_BUFFER_SIZE;
    bitmap_free(fs_buffer_bitmap, idx, FS_BUFFER_COUNT, &fs_buffer_next_hint);
}
