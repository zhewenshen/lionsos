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
#include <sddf/util/string.h>
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

static uint64_t request_id_stack[FS_QUEUE_CAPACITY - 1];
static int request_id_stack_top = -1;
static bool request_id_allocator_initialized = false;

static uint32_t request_bitmap = 0; // 32 concurrent conns
static int request_next_hint = 0;

#define FS_BUFFER_COUNT (FS_QUEUE_CAPACITY * 2)
#define FS_BITMAP_WORDS ((FS_BUFFER_COUNT + 63) / 64)
static uint64_t fs_buffer_bitmap[FS_BITMAP_WORDS];
static int fs_buffer_next_hint = 0;

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static void http_error(void *arg, err_t err);
static err_t http_poll(void *arg, struct tcp_pcb *pcb);

static void request_id_allocator_init(void)
{
    if (request_id_allocator_initialized)
        return;

    for (uint64_t id = 1; id < FS_QUEUE_CAPACITY; id++) {
        request_id_stack[++request_id_stack_top] = id;
    }
    request_id_allocator_initialized = true;
}

static uint64_t request_id_alloc(void)
{
    if (!request_id_allocator_initialized) {
        request_id_allocator_init();
    }

    if (request_id_stack_top < 0) {
        return 0;
    }

    return request_id_stack[request_id_stack_top--];
}

static void request_id_free(uint64_t id)
{
    if (id == 0 || !request_id_allocator_initialized)
        return;

    if (request_id_stack_top >= (int)(FS_QUEUE_CAPACITY - 2)) {
        return;
    }

    request_id_stack[++request_id_stack_top] = id;
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
    int start = request_next_hint;
    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++) {
        int idx = (start + i) % MAX_CONCURRENT_REQUESTS;
        uint32_t mask = 1U << idx;
        
        if (!(request_bitmap & mask)) {
            request_bitmap |= mask;
            request_next_hint = (idx + 1) % MAX_CONCURRENT_REQUESTS;
            
            memset(&requests[idx], 0, sizeof(http_request_t));
            requests[idx].in_use = true;
            requests[idx].file_fd = UINT64_MAX;
            return &requests[idx];
        }
    }
    return NULL;
}

static void request_free(http_request_t *req)
{
    if (!req || !req->in_use)
        return;

    if (req->file_open && req->file_fd != UINT64_MAX) {
        fs_cmd_t cmd = { .type = FS_CMD_FILE_CLOSE, .id = req->fs_request_id, .params.file_close.fd = req->file_fd };
        fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;
        fs_queue_publish_production(fs_command_queue, 1);
        microkit_notify(fs_config.server.id);
    }

    if (req->path_buffer >= 0) {
        fs_buffer_free(req->path_buffer);
    }
    if (req->read_buffer >= 0) {
        fs_buffer_free(req->read_buffer);
    }
    if (req->stat_buffer >= 0) {
        fs_buffer_free(req->stat_buffer);
    }

    if (req->fs_request_id != 0) {
        request_id_free(req->fs_request_id);
        req->fs_request_id = 0;
    }

    int idx = req - requests;
    if (idx >= 0 && idx < MAX_CONCURRENT_REQUESTS) {
        uint32_t mask = 1U << idx;
        request_bitmap &= ~mask;
        request_next_hint = idx;
    }
    req->in_use = false;
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
        send_http_error(req->pcb, 400, "Bad Request");
        req->state = REQUEST_STATE_CLOSING;
        return;
    } else if (pret == -2) {
        return;
    }

    if (method_len >= sizeof(req->method)) {
        send_http_error(req->pcb, 400, "Bad Request");
        req->state = REQUEST_STATE_CLOSING;
        return;
    }
    memcpy(req->method, method, method_len);
    req->method[method_len] = '\0';

    bool is_get = (method_len == 3 && memcmp(method, "GET", 3) == 0);
    bool is_head = (method_len == 4 && memcmp(method, "HEAD", 4) == 0);
    
    if (!is_get && !is_head) {
        send_http_error(req->pcb, 501, "Not Implemented");
        req->state = REQUEST_STATE_CLOSING;
        return;
    }
    
    req->is_head_request = is_head;

    const char *query = memchr(path, '?', path_len);
    size_t actual_path_len = query ? (size_t)(query - path) : path_len;

    if (actual_path_len == 0 || path[0] != '/') {
        send_http_error(req->pcb, 400, "Bad Request");
        req->state = REQUEST_STATE_CLOSING;
        return;
    }

    if (actual_path_len >= sizeof(req->path)) {
        send_http_error(req->pcb, 414, "URI Too Long");
        req->state = REQUEST_STATE_CLOSING;
        return;
    }
    memcpy(req->path, path, actual_path_len);
    req->path[actual_path_len] = '\0';

    int norm_result = normalize_path(req->path, actual_path_len);
    if (norm_result < 0) {
        send_http_error(req->pcb, 400, "Bad Request");
        req->state = REQUEST_STATE_CLOSING;
        return;
    }

    if (req->path[norm_result - 1] == '/') {
        if (norm_result + 10 >= sizeof(req->path)) {
            send_http_error(req->pcb, 414, "URI Too Long");
            req->state = REQUEST_STATE_CLOSING;
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

        switch (req->state) {
        case REQUEST_STATE_OPENING_FILE: {
            if (completion.status == FS_STATUS_SUCCESS) {
                req->file_fd = completion.data.file_open.fd;
                req->file_open = true;

                fs_cmd_t cmd = { .type = FS_CMD_FILE_SIZE,
                                 .id = req->fs_request_id,
                                 .params.file_size.fd = req->file_fd };
                fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;
                fs_queue_publish_production(fs_command_queue, 1);
                microkit_notify(fs_config.server.id);

                req->state = REQUEST_STATE_READING_FILE;
            } else {
                send_http_error(req->pcb, 404, "Not Found");
                req->state = REQUEST_STATE_CLOSING;
                tcp_close(req->pcb);
                request_free(req);
            }
            break;
        }

        case REQUEST_STATE_READING_FILE: {
            if (completion.status == FS_STATUS_SUCCESS) {
                if (req->file_size == 0) {
                    req->file_size = completion.data.file_size.size;

                    fs_cmd_t cmd = { .type = FS_CMD_FILE_READ,
                                     .id = req->fs_request_id,
                                     .params.file_read = {
                                         .fd = req->file_fd,
                                         .offset = 0,
                                         .buf = { .offset = req->read_buffer,
                                                  .size = MIN(FILE_READ_BUFFER_SIZE, req->file_size) } } };
                    fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;
                    fs_queue_publish_production(fs_command_queue, 1);
                    microkit_notify(fs_config.server.id);
                    return;
                }

                size_t bytes_read = completion.data.file_read.len_read;

                if (!req->headers_sent) {
                    int header_len = build_http_headers(req->response_headers, sizeof(req->response_headers),
                                                        content_type_from_extension(req->path), req->file_size);

                    if (header_len > 0) {
                        tcp_write(req->pcb, req->response_headers, header_len, TCP_WRITE_FLAG_MORE);
                        req->headers_sent = true;
                        
                        if (req->is_head_request) {
                            tcp_output(req->pcb);
                            req->state = REQUEST_STATE_CLOSING;
                            tcp_close(req->pcb);
                            request_free(req);
                            return;
                        }
                    }
                }

                if (bytes_read > 0) {
                    if (!req->is_head_request) {
                        tcp_write(req->pcb, fs_share + req->read_buffer, bytes_read, TCP_WRITE_FLAG_MORE);
                    }
                    req->file_offset += bytes_read;

                    if (req->file_offset < req->file_size) {
                        fs_cmd_t cmd = { .type = FS_CMD_FILE_READ,
                                         .id = req->fs_request_id,
                                         .params.file_read = {
                                             .fd = req->file_fd,
                                             .offset = req->file_offset,
                                             .buf = { .offset = req->read_buffer, .size = FILE_READ_BUFFER_SIZE } } };
                        fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;
                        fs_queue_publish_production(fs_command_queue, 1);
                        microkit_notify(fs_config.server.id);
                    } else {
                        tcp_output(req->pcb);
                        req->state = REQUEST_STATE_CLOSING;
                        tcp_close(req->pcb);
                        request_free(req);
                    }
                } else {
                    tcp_output(req->pcb);
                    req->state = REQUEST_STATE_CLOSING;
                    tcp_close(req->pcb);
                    request_free(req);
                }
            } else {
                send_http_error(req->pcb, 500, "Internal Server Error");
                req->state = REQUEST_STATE_CLOSING;
                tcp_close(req->pcb);
                request_free(req);
            }
            break;
        }

        default:
            break;
        }
    }

    fs_queue_publish_consumption(fs_completion_queue, to_consume);
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_request_t *req = (http_request_t *)arg;

    if (p == NULL) {
        if (req) {
            request_free(req);
        }
        tcp_close(pcb);
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

            if (req->state == REQUEST_STATE_OPENING_FILE) {
                req->path_buffer = fs_buffer_allocate();
                req->read_buffer = fs_buffer_allocate();

                snprintf(req->full_path, sizeof(req->full_path), "%s%s", WEB_ROOT_DIR, req->path);

                size_t path_len = strlen(req->full_path);
                memcpy(fs_share + req->path_buffer, req->full_path, path_len + 1);

                fs_cmd_t cmd = { .type = FS_CMD_FILE_OPEN,
                                 .id = req->fs_request_id,
                                 .params.file_open = { .path = { .offset = req->path_buffer, .size = path_len },
                                                       .flags = FS_OPEN_FLAGS_READ_ONLY } };
                fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;
                fs_queue_publish_production(fs_command_queue, 1);
                microkit_notify(fs_config.server.id);
            }
        }
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
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
    if (req) {
        request_free(req);
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

    sddf_lwip_init(&lib_sddf_lwip_config, &net_config, &timer_config, net_rx_queue, net_tx_queue, NULL,
                   netif_status_callback, NULL);
    
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
    fs_queue_idx_empty(fs_command_queue, 0)->cmd = cmd;
    fs_queue_publish_production(fs_command_queue, 1);
    microkit_notify(fs_config.server.id);
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
        sddf_dprintf("unknown channel notification: %lu\n", ch);
    }

    if (net_enabled) {
        sddf_lwip_maybe_notify();
    }
}

/* fs buffer management */
ptrdiff_t fs_buffer_allocate(void)
{
    int start = fs_buffer_next_hint;
    
    for (int i = 0; i < FS_BUFFER_COUNT; i++) {
        int idx = (start + i) % FS_BUFFER_COUNT;
        int word_idx = idx / 64;
        int bit_idx = idx % 64;
        uint64_t mask = 1ULL << bit_idx;
        
        if (!(fs_buffer_bitmap[word_idx] & mask)) {
            fs_buffer_bitmap[word_idx] |= mask;
            fs_buffer_next_hint = (idx + 1) % FS_BUFFER_COUNT;

            return idx * FILE_READ_BUFFER_SIZE;
        }
    }
    return -1;
}

void fs_buffer_free(ptrdiff_t buffer)
{
    int idx = buffer / FILE_READ_BUFFER_SIZE;
    if (idx >= 0 && idx < FS_BUFFER_COUNT) {
        int word_idx = idx / 64;
        int bit_idx = idx % 64;
        uint64_t mask = 1ULL << bit_idx;
        
        fs_buffer_bitmap[word_idx] &= ~mask;
        fs_buffer_next_hint = idx;
    }
}
