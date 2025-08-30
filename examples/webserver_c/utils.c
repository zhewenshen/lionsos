/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

typedef enum { STATE_NORMAL = 0, STATE_AFTER_SLASH = 1 } parse_state_t;

static inline int hex_char_to_int(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int normalize_path(char *path, size_t len)
{
    if (!path || len == 0)
        return -1;

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

#define SECONDS_1970_TO_2000 946684800ULL

static inline int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static void seconds_to_struct_time(uint64_t seconds, int *year, int *month, int *day, int *hour, int *minute,
                                   int *second, int *weekday)
{
    static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    *second = seconds % 60;
    seconds /= 60;
    *minute = seconds % 60;
    seconds /= 60;
    *hour = seconds % 24;
    seconds /= 24;

    *weekday = (seconds + 6) % 7;

    *year = 2000;
    while (true) {
        int days_this_year = is_leap_year(*year) ? 366 : 365;
        if (seconds < days_this_year)
            break;
        seconds -= days_this_year;
        (*year)++;
    }

    *month = 1;
    while (true) {
        int days_this_month = days_in_month[*month - 1];
        if (*month == 2 && is_leap_year(*year))
            days_this_month = 29;
        if (seconds < days_this_month)
            break;
        seconds -= days_this_month;
        (*month)++;
    }

    *day = seconds + 1;
}

void format_http_date_from_unix(char *buffer, size_t buffer_size, uint64_t unix_timestamp)
{
    const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    uint64_t seconds_since_2000 = unix_timestamp - SECONDS_1970_TO_2000;

    int year, month, day, hour, minute, second, weekday;
    seconds_to_struct_time(seconds_since_2000, &year, &month, &day, &hour, &minute, &second, &weekday);

    snprintf(buffer, buffer_size, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[weekday], day, months[month - 1], year,
             hour, minute, second);
}

uint64_t parse_http_date(const char *date_str)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    char day_name[4], month_name[4];
    int day, year, hour, minute, second;

    if (sscanf(date_str, "%3s, %d %3s %d %d:%d:%d", day_name, &day, month_name, &year, &hour, &minute, &second) != 7) {
        return 0;
    }

    int month = 1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(month_name, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }

    uint64_t seconds_since_2000 = 0;
    for (int y = 2000; y < year; y++) {
        seconds_since_2000 += is_leap_year(y) ? 366 * 86400ULL : 365 * 86400ULL;
    }

    static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    for (int m = 1; m < month; m++) {
        int days = days_in_month[m - 1];
        if (m == 2 && is_leap_year(year))
            days = 29;
        seconds_since_2000 += days * 86400ULL;
    }

    seconds_since_2000 += (day - 1) * 86400ULL + hour * 3600ULL + minute * 60ULL + second;
    return seconds_since_2000 + SECONDS_1970_TO_2000;
}
