/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "utils.h"

typedef enum {
    STATE_NORMAL = 0,
    STATE_AFTER_SLASH = 1
} parse_state_t;

static inline int hex_char_to_int(char c) 
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int normalize_path(char *path, size_t len)
{
    if (!path || len == 0) return -1;
    
    char *src = path;
    char *dst = path;
    const char *end = path + len;
    
    char prev_char = 0;
    parse_state_t state = STATE_NORMAL;
    
    if (*src != '/') {
        return -1;
    }
    
    *dst++ = *src++;
    prev_char = '/';
    state = STATE_AFTER_SLASH;
    
    while (src < end && *src) {
        char c = *src++;
        
        switch (c) {
            case '/': {
                if (prev_char != '/') {
                    *dst++ = '/';
                    prev_char = '/';
                    state = STATE_AFTER_SLASH;
                }
                break;
            }
            case '.': {
                if (state == STATE_AFTER_SLASH) {
                    if (src < end && *src == '.') {
                        if (src + 1 >= end || src[1] == '/' || src[1] == '\0' || src[1] == '?') {
                            return -1;
                        }
                    }
                    
                    if (src < end && *src == '/') {
                        src++;
                        state = STATE_AFTER_SLASH;
                    } else {
                        *dst++ = c;
                        prev_char = c;
                        state = STATE_NORMAL;
                    }
                } else {
                    *dst++ = c;
                    prev_char = c;
                    state = STATE_NORMAL;
                }
                break;
            }
            case '%': {
                if (src + 1 < end) {
                    int high = hex_char_to_int(src[0]);
                    int low = hex_char_to_int(src[1]);
                    
                    if (high >= 0 && low >= 0) {
                        char decoded = (char)((high << 4) | low);
                        
                        if (decoded == 0 || decoded < 32 || decoded == 127) {
                            return -1;
                        }
                        
                        if (decoded == '/' || decoded == '\\') {
                            return -1;
                        }
                        
                        *dst++ = decoded;
                        prev_char = decoded;
                        src += 2;
                    } else {
                        *dst++ = c;
                        prev_char = c;
                    }
                } else {
                    *dst++ = c;
                    prev_char = c;
                }
                state = STATE_NORMAL;
                break;
            }
            case '?':
            case '#': {
                goto done;
            }
            default: {
                if (c >= 32 && c != 127) {
                    *dst++ = c;
                    prev_char = c;
                    state = STATE_NORMAL;
                } else {
                    return -1;
                }
                break;
            }
        }
    }
    
done:
    if (dst == path) {
        *dst++ = '/';
    }
    
    *dst = '\0';
    return dst - path;
}
