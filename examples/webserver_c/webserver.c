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
#include <stdio.h>
#include <string.h>

#include "picohttpparser.h"
#include "utils.h"

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

static http_request_t requests[MAX_CONCURRENT_REQUESTS];
static bool fs_initialized = false;

static uint64_t id_counter = 1;

static uint64_t request_bitmap[2] = { 0 };
static int request_next_hint = 0;

#define FS_BUFFER_COUNT (FS_QUEUE_CAPACITY * 2)
#define FS_BITMAP_WORDS ((FS_BUFFER_COUNT + 63) / 64)

#define FS_OP_SAFE(req) (!req->connection_closed && !req->fs_operation_in_flight)

#define HTTP_ERROR_AND_CLOSE(req, code, msg) do { \
    if (req->pcb) send_http_error(req->pcb, code, msg); \
    request_close_connection(req); \
} while(0)

#define BITMAP_WORD(idx) ((idx) / 64)
#define BITMAP_BIT(idx) ((idx) % 64)
#define BITMAP_MASK(idx) (1ULL << BITMAP_BIT(idx))

#define SUBMIT_FS_CMD(cmd) do { \
    fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd; \
    fs_queue_publish_production(fs_command_queue, 1); \
    microkit_notify(fs_config.server.id); \
} while(0)

static uint64_t fs_buffer_bitmap[FS_BITMAP_WORDS];
static int fs_buffer_next_hint = 0;

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static void http_error(void *arg, err_t err);
static err_t http_poll(void *arg, struct tcp_pcb *pcb);
static void request_start_operation(http_request_t *req);
static void request_close_connection(http_request_t *req);

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
    if (req->pcb) {
        tcp_output(req->pcb);
    }
    request_close_connection(req);
}

static uint64_t request_id_alloc(void)
{
    uint64_t id = id_counter++;
    if (id == 0) id = id_counter++;  /* skip 0 after wrap */
    return id;
}


static void request_start_operation(http_request_t *req)
{
    if (req && req->in_use) {
        req->outstanding_operations++;
        req->fs_operation_in_flight = true;
    }
}

static void request_complete_operation(http_request_t *req)
{
    if (req && req->in_use) {
        req->outstanding_operations--;
        req->fs_operation_in_flight = false;
    }
}

static bool request_can_cleanup(http_request_t *req)
{
    return req && req->in_use && req->connection_closed && req->outstanding_operations == 0;
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

static http_request_t *request_alloc(void)
{
    int idx = bitmap_alloc(request_bitmap, MAX_CONCURRENT_REQUESTS, &request_next_hint);
    if (idx == -1)
        return NULL;

    memset(&requests[idx], 0, sizeof(http_request_t));
    requests[idx].in_use = true;
    requests[idx].file_fd = UINT64_MAX;
    requests[idx].outstanding_operations = 0;
    requests[idx].connection_closed = false;
    requests[idx].fs_operation_in_flight = false;
    return &requests[idx];
}

static void request_close_connection(http_request_t *req)
{
    if (!req || !req->in_use)
        return;

    if (req->pcb) {
        tcp_cleanup_callbacks(req->pcb);
        tcp_close(req->pcb);
        req->pcb = NULL;
    }

    req->connection_closed = true;
}

static void request_free(http_request_t *req)
{
    if (!req || !req->in_use || !request_can_cleanup(req))
        return;

    if (req->file_open && req->file_fd != UINT64_MAX && req->fs_request_id > 0) {
        fs_cmd_t cmd = { .type = FS_CMD_FILE_CLOSE, .id = req->fs_request_id, .params.file_close.fd = req->file_fd };
        SUBMIT_FS_CMD(cmd);
    }

    free_request_buffers(req);

    int idx = req - requests;
    bitmap_free(request_bitmap, idx, MAX_CONCURRENT_REQUESTS, &request_next_hint);

    req->in_use = false;
}

static void request_cleanup_if_ready(http_request_t *req)
{
    if (request_can_cleanup(req)) {
        request_free(req);
    }
}

static int build_http_headers(char *buffer, size_t buffer_size, const char *content_type, uint64_t content_length)
{
    char *pos = buffer;
    const char *end = buffer + buffer_size - 1;

    const size_t prefix_len = sizeof(HTTP_HEADER_PREFIX) - 1;
    if (pos + prefix_len > end)
        return -1;
    memcpy(pos, HTTP_HEADER_PREFIX, prefix_len);
    pos += prefix_len;

    size_t ct_len = strlen(content_type);
    if (pos + ct_len > end)
        return -1;
    memcpy(pos, content_type, ct_len);
    pos += ct_len;

    const size_t suffix_len = sizeof(HTTP_HEADER_SUFFIX) - 1;
    if (pos + suffix_len > end)
        return -1;
    memcpy(pos, HTTP_HEADER_SUFFIX, suffix_len);
    pos += suffix_len;

    char length_str[32];
    int length_digits = snprintf(length_str, sizeof(length_str), "%lu", content_length);
    if (pos + length_digits > end)
        return -1;
    memcpy(pos, length_str, length_digits);
    pos += length_digits;

    const size_t end_len = sizeof(HTTP_HEADER_END) - 1;
    if (pos + end_len > end)
        return -1;
    memcpy(pos, HTTP_HEADER_END, end_len);
    pos += end_len;

    return pos - buffer;
}

static void send_http_error(struct tcp_pcb *pcb, int code, const char *status)
{
    static char response[256];
    int len = snprintf(response, sizeof(response),
                       "HTTP/1.0 %d %s\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: %d\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%d %s\n",
                       code, status, strlen(status) + 5, code, status);

    tcp_write(pcb, response, len, TCP_WRITE_FLAG_MORE);
    tcp_output(pcb);
}

static void parse_http_request(http_request_t *req)
{
    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[16];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int pret = phr_parse_request(req->header_buffer, req->header_len, &method, &method_len, &path, &path_len,
                                 &minor_version, headers, &num_headers, 0);

    if (pret == -1) {
        HTTP_ERROR_AND_CLOSE(req, 400, "Bad Request");
        return;
    } else if (pret == -2) {
        return;
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
        HTTP_ERROR_AND_CLOSE(req, 501, "Not Implemented");
        return;
    }

    req->is_head_request = is_head;

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
            send_http_error(req->pcb, 414, "URI Too Long");
            request_close_connection(req);
            return;
        }
        strcat(req->path, "index.html");
    }

    snprintf(req->version, sizeof(req->version), "1.%d", minor_version);

    req->state = REQUEST_STATE_OPENING_FILE;
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
            if (requests[j].in_use && requests[j].fs_request_id == completion.id) {
                req = &requests[j];
                break;
            }
        }

        if (!req) {
            continue;
        }

        request_complete_operation(req);

        if (!req->in_use) {
            continue;
        }

        if (req->connection_closed) {
            request_cleanup_if_ready(req);
            continue;
        }

        switch (req->state) {
        case REQUEST_STATE_OPENING_FILE: {
            if (completion.status == FS_STATUS_SUCCESS) {
                req->file_fd = completion.data.file_open.fd;
                req->file_open = true;

                if (FS_OP_SAFE(req)) {
                    request_start_operation(req);
                    fs_cmd_t cmd = { .type = FS_CMD_FILE_SIZE,
                                     .id = req->fs_request_id,
                                     .params.file_size.fd = req->file_fd };
                    SUBMIT_FS_CMD(cmd);
                    req->state = REQUEST_STATE_READING_FILE;
                } else {
                    request_close_connection(req);
                }
            } else {
                HTTP_ERROR_AND_CLOSE(req, 404, "Not Found");
            }
            break;
        }

        case REQUEST_STATE_READING_FILE: {
            if (completion.status == FS_STATUS_SUCCESS) {
                if (req->file_size == 0) {
                    req->file_size = completion.data.file_size.size;

                    if (FS_OP_SAFE(req)) {
                        send_file_read_command(req, 0, MIN(FILE_READ_BUFFER_SIZE, req->file_size));
                    } else {
                        request_close_connection(req);
                    }
                    break;
                }

                size_t bytes_read = completion.data.file_read.len_read;

                if (!req->headers_sent && req->pcb) {
                    int header_len = build_http_headers(req->response_headers, sizeof(req->response_headers),
                                                        content_type_from_extension(req->path), req->file_size);

                    if (header_len > 0) {
                        tcp_write(req->pcb, req->response_headers, header_len, TCP_WRITE_FLAG_MORE);
                        req->headers_sent = true;

                        if (req->is_head_request) {
                            finish_response_and_close(req);
                            break;
                        }
                    }
                }

                if (bytes_read > 0) {
                    if (!req->is_head_request && req->pcb) {
                        tcp_write(req->pcb, fs_share + req->read_buffer, bytes_read, TCP_WRITE_FLAG_MORE);
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
            break;
        }

        default:
            request_close_connection(req);
            break;
        }
    }

    fs_queue_publish_consumption(fs_completion_queue, to_consume);

    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++) {
        if (request_can_cleanup(&requests[i])) {
            request_free(&requests[i]);
        }
    }
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_request_t *req = (http_request_t *)arg;

    if (!req || !req->in_use || req->connection_closed) {
        if (p != NULL) {
            pbuf_free(p);
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    if (p == NULL) {
        if (req) {
            request_close_connection(req);
        }
        return ERR_OK;
    }

    if (err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return err;
    }

    if (req->state == REQUEST_STATE_READING_HEADER) {
        size_t copy_len = p->tot_len;
        if (req->header_len + copy_len > sizeof(req->header_buffer) - 1) {
            copy_len = sizeof(req->header_buffer) - 1 - req->header_len;
        }

        pbuf_copy_partial(p, req->header_buffer + req->header_len, copy_len, 0);
        req->header_len += copy_len;
        req->header_buffer[req->header_len] = '\0';

        if (strstr(req->header_buffer, "\r\n\r\n")) {
            req->state = REQUEST_STATE_PARSING;
            parse_http_request(req);

            if (req->state == REQUEST_STATE_OPENING_FILE && FS_OP_SAFE(req)) {
                req->path_buffer = fs_buffer_allocate();
                req->read_buffer = fs_buffer_allocate();

                snprintf(req->full_path, sizeof(req->full_path), "%s%s", WEB_ROOT_DIR, req->path);

                size_t path_len = strlen(req->full_path);
                memcpy(fs_share + req->path_buffer, req->full_path, path_len + 1);

                request_start_operation(req);
                fs_cmd_t cmd = { .type = FS_CMD_FILE_OPEN,
                                 .id = req->fs_request_id,
                                 .params.file_open = { .path = { .offset = req->path_buffer, .size = path_len },
                                                       .flags = FS_OPEN_FLAGS_READ_ONLY } };
                SUBMIT_FS_CMD(cmd);
            } else if (req->state == REQUEST_STATE_OPENING_FILE) {
                request_close_connection(req);
            }
        }
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (!newpcb) {
        return ERR_ABRT;
    }

    http_request_t *req = request_alloc();
    if (!req) {
        tcp_close(newpcb);
        return ERR_MEM;
    }

    req->pcb = newpcb;
    req->state = REQUEST_STATE_READING_HEADER;
    req->path_buffer = -1;
    req->read_buffer = -1;
    req->stat_buffer = -1;

    req->fs_request_id = request_id_alloc();
    if (req->fs_request_id == 0) {
        request_free(req);
        tcp_close(newpcb);
        return ERR_MEM;
    }

    /* tcp_nodelay for lower latency */
    tcp_nagle_disable(newpcb);

    tcp_arg(newpcb, req);
    tcp_recv(newpcb, http_recv);
    tcp_err(newpcb, http_error);
    tcp_poll(newpcb, http_poll, 0);

    return ERR_OK;
}

static void http_error(void *arg, err_t err)
{
    http_request_t *req = (http_request_t *)arg;
    if (req && req->in_use && !req->connection_closed) {
        req->pcb = NULL;
        request_close_connection(req);
    }
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb)
{
    return ERR_OK;
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
        // sddf_dprintf("Failed to bind TCP PCB: %d\n", err);
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
