/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sddf/util/printf.h>

typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uint64_t u64_t;
typedef int64_t s64_t;

typedef uintptr_t mem_ptr_t;

#ifndef SSIZE_MAX
/* Whilst ssize_t is defined by sys/types.h, at least on aarch64-none-elf GCC
   version 14.2.1 SSIZE_MAX is not defined.

   If SSIZE_MAX is not defined then we take (SIZE_MAX - 1)/2 under the
   assumption that ssize_t and size_t are related in the standard way.
*/
#define SSIZE_MAX ((SIZE_MAX - 1) >> 1)
#endif

#define LWIP_ERR_T int

#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

#define LWIP_PLATFORM_DIAG(x)                                                  \
        do {                                                                   \
            sddf_dprintf x ;                                                   \
        } while(0)

#define LWIP_PLATFORM_ASSERT(x)                                                \
        do {                                                                   \
            if (!x) {                                                          \
                sddf_dprintf("assertion violated: %s : %s:%d:%s\n",            \
                       #x, __FILE__, __LINE__, __FUNCTION__);                  \
                while(1);                                                      \
            }                                                                  \
        } while(0)

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#define LWIP_RAND() ((u32_t)rand())

#define LWIP_NO_LIMITS_H 1

int rand(void);