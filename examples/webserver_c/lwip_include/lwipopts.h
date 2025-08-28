/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            0

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (1024 * 1024)

#define MEMP_NUM_PBUF                   512
#define MEMP_NUM_TCP_PCB                32
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                256

#define PBUF_POOL_SIZE                  512
#define PBUF_POOL_BUFSIZE               1600

#define TCP_MSS                         1460
#define TCP_WND                         (32 * TCP_MSS)
#define TCP_SND_BUF                     (16 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)

#define LWIP_DHCP                       1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_ICMP                       1

#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

#define LWIP_DBG_MIN_LEVEL              LWIP_DBG_LEVEL_WARNING
#define LWIP_DEBUG                      0

#define ETHARP_SUPPORT_STATIC_ENTRIES   1

#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1

#define LWIP_NETIF_STATUS_CALLBACK     1
#define LWIP_NETIF_LINK_CALLBACK       1

#define LWIP_PBUF_CUSTOM_DATA \
    u8_t in_use; \
    struct pbuf *next_chain;

#define LWIP_PBUF_INIT_CUSTOM_DATA(p) \
    (p)->in_use = 0; \
    (p)->next_chain = NULL;