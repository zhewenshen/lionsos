/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uint64_t u64_t;
typedef int64_t s64_t;

typedef uintptr_t mem_ptr_t;

#define LWIP_ERR_T int

#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

#define LWIP_PLATFORM_DIAG(x) do { sddf_printf_ x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { sddf_printf_("Assertion \"%s\" failed at line %d in %s\n", x, __LINE__, __FILE__); for(;;); } while(0)

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#define LWIP_RAND() ((u32_t)rand())

int sddf_printf_(const char *format, ...) __attribute__((format(__printf__, 1, 2)));
int rand(void);