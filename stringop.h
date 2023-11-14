#pragma once

#include <stdio.h>

#ifdef __GNUC__
#define _SWAY_ATTRIB_PRINTF(start, end) __attribute__((format(printf, start, end)))
#else
#define _SWAY_ATTRIB_PRINTF(start, end)
#endif


char *vformat_str(const char *fmt, va_list args) _SWAY_ATTRIB_PRINTF(1, 0);
char *format_str(const char *fmt, ...) _SWAY_ATTRIB_PRINTF(1, 2);
