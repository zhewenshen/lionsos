/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdlib.h>
#include <stdbool.h>
#include <sddf/network/constants.h>

#define NO_SYS 1
#define LWIP_TIMERS 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_IPV4 1
#define LWIP_ICMP 1
#define LWIP_IGMP 1
#define LWIP_DNS 1
#define LWIP_DNS_SECURE (LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT)
#define LWIP_DHCP 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE 0x30000
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETIF_STATUS_CALLBACK 1

#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_TCP 0
#define CHECKSUM_CHECK_ICMP 0
#define CHECKSUM_CHECK_ICMP6 0

#ifdef NETWORK_HW_HAS_CHECKSUM
#define CHECKSUM_GEN_IP 0
#define CHECKSUM_GEN_UDP 0
#define CHECKSUM_GEN_TCP 0
#define CHECKSUM_GEN_ICMP 0
#define CHECKSUM_GEN_ICMP6 0
#else
#define CHECKSUM_GEN_IP 1
#define CHECKSUM_GEN_UDP 1
#define CHECKSUM_GEN_TCP 1
#define CHECKSUM_GEN_ICMP 1
#define CHECKSUM_GEN_ICMP6 1
#endif

#define TCP_MSS 1460
#define TCP_WND (1000 * TCP_MSS)
#define TCP_SND_BUF TCP_WND
#define TCP_SNDLOWAT TCP_MSS
#define TCP_QUEUE_OOSEQ 1
#define LWIP_TCP_SACK_OUT 1
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 12
#define LWIP_TCP_TIMESTAMPS 1

#define PBUF_POOL_SIZE 1000
#define MEMP_NUM_PBUF (10 * TCP_SND_QUEUELEN)
#define MEMP_NUM_TCP_SEG (10 * TCP_SND_QUEUELEN)
#define MEMP_NUM_TCP_PCB 100
#define MEMP_NUM_TCP_PCB_LISTEN 100
#define MEMP_NUM_NETCONN 100

#define LWIP_STATS 0

#define LWIP_PBUF_CUSTOM_DATA \
    u8_t in_use; \
    struct pbuf *next_chain;

#define LWIP_PBUF_INIT_CUSTOM_DATA(p) \
    (p)->in_use = 0; \
    (p)->next_chain = NULL;

