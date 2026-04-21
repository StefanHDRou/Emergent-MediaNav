/*
 * debug_log_portable.c - Portable logging (printf-based)
 */
#ifdef PORTABLE_BUILD

#include <stdio.h>
#include <stdarg.h>
#include "types_portable.h"

/* Stub: logging is compiled out in portable builds unless DEBUG */
void mn1_log_init(void) {}
void mn1_log_shutdown(void) {}

#if MN1_LOG_LEVEL >= 1
void mn1_log_write(int level, const char* fmt, ...)
{
    static const char* levelNames[] = { "", "ERR", "WRN", "INF" };
    va_list args;
    printf("[%s] ", levelNames[level]);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}
#endif

#endif /* PORTABLE_BUILD */
