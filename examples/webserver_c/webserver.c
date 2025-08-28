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

__attribute__((__section__(".serial_client_config"))) serial_client_config_t serial_config;
__attribute__((__section__(".timer_client_config"))) timer_client_config_t timer_config;
__attribute__((__section__(".net_client_config"))) net_client_config_t net_config;
__attribute__((__section__(".fs_client_config"))) fs_client_config_t fs_config;

serial_queue_handle_t serial_tx_queue_handle;
net_queue_handle_t net_rx_queue;
net_queue_handle_t net_tx_queue;

fs_queue_t *fs_command_queue;
fs_queue_t *fs_completion_queue;
char *fs_share;

typedef struct pbuf_custom_offset {
    struct pbuf_custom custom;
    size_t offset;
} pbuf_custom_offset_t;

typedef struct network_state {
    struct netif netif;
    uint8_t mac[6];
} network_state_t;

static network_state_t net_state;
static bool notify_tx = false;
static bool notify_rx = false;
static bool net_enabled = false;

LWIP_MEMPOOL_DECLARE(RX_POOL, 512 * 2, sizeof(pbuf_custom_offset_t), "zero-copy RX pool");

struct pbuf *pbuf_head;
struct pbuf *pbuf_tail;

static http_request_t requests[MAX_CONCURRENT_REQUESTS];
static bool fs_initialized = false;

static uint64_t request_id_stack[FS_QUEUE_CAPACITY - 1];
static int request_id_stack_top = -1;
static bool request_id_allocator_initialized = false;

static http_request_t *request_free_list = NULL;
static bool request_allocator_initialized = false;

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

static void request_allocator_init(void)
{
    if (request_allocator_initialized)
        return;

    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++) {
        requests[i].pcb = (struct tcp_pcb *)request_free_list;
        request_free_list = &requests[i];
    }
    request_allocator_initialized = true;
}

static http_request_t *request_alloc(void)
{
    if (!request_allocator_initialized) {
        request_allocator_init();
    }

    if (request_free_list) {
        http_request_t *req = request_free_list;
        request_free_list = (http_request_t *)req->pcb;
        memset(req, 0, sizeof(http_request_t));
        req->in_use = true;
        req->file_fd = UINT64_MAX;
        return req;
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

    req->pcb = (struct tcp_pcb *)request_free_list;
    request_free_list = req;
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

    if (actual_path_len > 0 && path[actual_path_len - 1] == '/') {
        int result = snprintf(req->path, sizeof(req->path), "%.*sindex.html", (int)actual_path_len, path);
        if (result >= sizeof(req->path)) {
            send_http_error(req->pcb, 414, "URI Too Long");
            req->state = REQUEST_STATE_CLOSING;
            return;
        }
    } else {
        if (actual_path_len >= sizeof(req->path)) {
            send_http_error(req->pcb, 414, "URI Too Long");
            req->state = REQUEST_STATE_CLOSING;
            return;
        }
        memcpy(req->path, path, actual_path_len);
        req->path[actual_path_len] = '\0';
    }

    snprintf(req->version, sizeof(req->version), "1.%d", minor_version);

    if (strstr(req->path, "..") != NULL) {
        send_http_error(req->pcb, 404, "Not Found");
        req->state = REQUEST_STATE_CLOSING;
        return;
    }

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

                if (net_enabled && dhcp_supplied_address(&net_state.netif)) {
                    static bool http_server_started = false;
                    if (!http_server_started) {
                        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
                        if (pcb != NULL) {
                            err_t error = tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT);
                            if (!error) {
                                pcb = tcp_listen_with_backlog_and_err(pcb, 8, &error);
                                if (!error) {
                                    tcp_accept(pcb, http_accept);
                                    http_server_started = true;
                                }
                            }
                        }
                    }
                }
            } else {
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

u32_t sys_now(void)
{
    static u32_t time_counter = 0;
    time_counter += 10;
    return time_counter;
}

static void interface_free_buffer(struct pbuf *buf)
{
    SYS_ARCH_DECL_PROTECT(old_level);
    pbuf_custom_offset_t *custom_pbuf_offset = (pbuf_custom_offset_t *)buf;
    SYS_ARCH_PROTECT(old_level);
    net_buff_desc_t buffer = { custom_pbuf_offset->offset, 0 };
    net_enqueue_free(&net_rx_queue, buffer);
    notify_rx = true;
    LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf_offset);
    SYS_ARCH_UNPROTECT(old_level);
}

static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    err_t ret = ERR_OK;

    if (p->tot_len > NET_BUFFER_SIZE) {
        return ERR_MEM;
    }

    net_buff_desc_t buffer;
    int err = net_dequeue_free(&net_tx_queue, &buffer);
    if (err) {
        return ERR_MEM;
    }

    unsigned char *frame = (unsigned char *)(buffer.io_or_offset + net_config.tx_data.vaddr);
    unsigned int copied = 0;
    for (struct pbuf *curr = p; curr != NULL; curr = curr->next) {
        memcpy(frame + copied, curr->payload, curr->len);
        copied += curr->len;
    }

    buffer.len = copied;
    err = net_enqueue_active(&net_tx_queue, buffer);
    assert(!err);
    notify_tx = true;

    return ret;
}

static void netif_status_callback(struct netif *netif)
{
    static bool http_server_started = false;

    if (dhcp_supplied_address(netif) && !http_server_started) {
        if (fs_initialized) {
            struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
            if (pcb == NULL) {
                return;
            }

            err_t error = tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT);
            if (error) {
                return;
            }

            pcb = tcp_listen_with_backlog_and_err(pcb, 8, &error);
            if (error) {
                return;
            }

            tcp_accept(pcb, http_accept);
            http_server_started = true;
        }
    }
}

static err_t ethernet_init(struct netif *netif)
{
    if (netif->state == NULL) {
        return ERR_ARG;
    }

    network_state_t *data = netif->state;

    netif->hwaddr[0] = data->mac[0];
    netif->hwaddr[1] = data->mac[1];
    netif->hwaddr[2] = data->mac[2];
    netif->hwaddr[3] = data->mac[3];
    netif->hwaddr[4] = data->mac[4];
    netif->hwaddr[5] = data->mac[5];
    netif->mtu = ETHER_MTU;
    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    netif->output = etharp_output;
    netif->linkoutput = netif_output;
    NETIF_INIT_SNMP(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;

    return ERR_OK;
}

static void init_networking(void)
{
    net_queue_init(&net_rx_queue, net_config.rx.free_queue.vaddr, net_config.rx.active_queue.vaddr,
                   net_config.rx.num_buffers);
    net_queue_init(&net_tx_queue, net_config.tx.free_queue.vaddr, net_config.tx.active_queue.vaddr,
                   net_config.tx.num_buffers);
    net_buffers_init(&net_tx_queue, 0);

    lwip_init();
    LWIP_MEMPOOL_INIT(RX_POOL);

    for (int i = 0; i < 6; i++) {
        net_state.mac[i] = net_config.mac_addr[i];
    }

    struct ip4_addr netmask, ipaddr, gw, multicast;
    ipaddr_aton("0.0.0.0", &gw);
    ipaddr_aton("0.0.0.0", &ipaddr);
    ipaddr_aton("0.0.0.0", &multicast);
    ipaddr_aton("255.255.255.0", &netmask);

    net_state.netif.name[0] = 'e';
    net_state.netif.name[1] = '0';

    if (!netif_add(&(net_state.netif), &ipaddr, &netmask, &gw, &net_state, ethernet_init, ethernet_input)) {}
    netif_set_default(&(net_state.netif));
    netif_set_status_callback(&(net_state.netif), netif_status_callback);
    netif_set_up(&(net_state.netif));

    int err = dhcp_start(&(net_state.netif));
    if (err) {}

    if (notify_rx && net_require_signal_free(&net_rx_queue)) {
        net_cancel_signal_free(&net_rx_queue);
        notify_rx = false;
        if (!microkit_have_signal) {
            microkit_deferred_notify(net_config.rx.id);
        } else if (microkit_signal_cap != BASE_OUTPUT_NOTIFICATION_CAP + net_config.rx.id) {
            microkit_notify(net_config.rx.id);
        }
    }

    if (notify_tx && net_require_signal_active(&net_tx_queue)) {
        net_cancel_signal_active(&net_tx_queue);
        notify_tx = false;
        if (!microkit_have_signal) {
            microkit_deferred_notify(net_config.tx.id);
        } else if (microkit_signal_cap != BASE_OUTPUT_NOTIFICATION_CAP + net_config.tx.id) {
            microkit_notify(net_config.tx.id);
        }
    }
}

static void process_rx_packets(void)
{
    bool reprocess = true;
    while (reprocess) {
        while (!net_queue_empty_active(&net_rx_queue)) {
            net_buff_desc_t buffer;
            net_dequeue_active(&net_rx_queue, &buffer);

            pbuf_custom_offset_t *custom_pbuf_offset = (pbuf_custom_offset_t *)LWIP_MEMPOOL_ALLOC(RX_POOL);
            custom_pbuf_offset->offset = buffer.io_or_offset;
            custom_pbuf_offset->custom.custom_free_function = interface_free_buffer;

            struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, buffer.len, PBUF_REF, &custom_pbuf_offset->custom,
                                                 (void *)(buffer.io_or_offset + net_config.rx_data.vaddr),
                                                 NET_BUFFER_SIZE);

            if (net_state.netif.input(p, &net_state.netif) != ERR_OK) {
                pbuf_free(p);
            }
        }

        net_request_signal_active(&net_rx_queue);
        reprocess = false;

        if (!net_queue_empty_active(&net_rx_queue)) {
            net_cancel_signal_active(&net_rx_queue);
            reprocess = true;
        }
    }
}

static void handle_network_notifications(void)
{
    if (notify_rx && net_require_signal_free(&net_rx_queue)) {
        net_cancel_signal_free(&net_rx_queue);
        notify_rx = false;
        if (!microkit_have_signal) {
            microkit_deferred_notify(net_config.rx.id);
        } else if (microkit_signal_cap != BASE_OUTPUT_NOTIFICATION_CAP + net_config.rx.id) {
            microkit_notify(net_config.rx.id);
        }
    }

    if (notify_tx && net_require_signal_active(&net_tx_queue)) {
        net_cancel_signal_active(&net_tx_queue);
        notify_tx = false;
        if (!microkit_have_signal) {
            microkit_deferred_notify(net_config.tx.id);
        } else if (microkit_signal_cap != BASE_OUTPUT_NOTIFICATION_CAP + net_config.tx.id) {
            microkit_notify(net_config.tx.id);
        }
    }
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
        process_rx_packets();
    } else if (net_enabled && ch == net_config.tx.id) {
    } else if (ch == timer_config.driver_id) {
        if (net_enabled) {
            sys_check_timeouts();
        }
    } else if (ch == fs_config.server.id) {
        process_file_operations();
    } else if (ch == serial_config.tx.id) {
    } else {
    }

    if (net_enabled) {
        handle_network_notifications();
    }
}

/* fs buffer management */
static ptrdiff_t fs_buffer_pool[FS_QUEUE_CAPACITY * 2];
static bool fs_buffer_used[FS_QUEUE_CAPACITY * 2];

ptrdiff_t fs_buffer_allocate(void)
{
    for (int i = 0; i < FS_QUEUE_CAPACITY * 2; i++) {
        if (!fs_buffer_used[i]) {
            fs_buffer_used[i] = true;
            return i * FILE_READ_BUFFER_SIZE;
        }
    }
    return -1;
}

void fs_buffer_free(ptrdiff_t buffer)
{
    int index = buffer / FILE_READ_BUFFER_SIZE;
    if (index >= 0 && index < FS_QUEUE_CAPACITY * 2) {
        fs_buffer_used[index] = false;
    }
}
