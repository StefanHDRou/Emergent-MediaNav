/*
 * debug_log.h - Minimal Debug Logging
 */

#ifndef MN1AA_DEBUG_LOG_H
#define MN1AA_DEBUG_LOG_H

#include "config.h"
#include "types.h"

/* Initialize logging (opens file or serial port) */
void mn1_log_init(void);

/* Shutdown logging */
void mn1_log_shutdown(void);

/* Log functions - compiled out entirely when MN1_LOG_LEVEL == 0 */
#if MN1_LOG_LEVEL >= 1
  void mn1_log_write(int level, const WCHAR* fmt, ...);
  #define MN1_LOG_ERROR(fmt, ...)   mn1_log_write(1, fmt, ##__VA_ARGS__)
#else
  #define MN1_LOG_ERROR(fmt, ...)   ((void)0)
#endif

#if MN1_LOG_LEVEL >= 2
  #define MN1_LOG_WARN(fmt, ...)    mn1_log_write(2, fmt, ##__VA_ARGS__)
#else
  #define MN1_LOG_WARN(fmt, ...)    ((void)0)
#endif

#if MN1_LOG_LEVEL >= 3
  #define MN1_LOG_INFO(fmt, ...)    mn1_log_write(3, fmt, ##__VA_ARGS__)
#else
  #define MN1_LOG_INFO(fmt, ...)    ((void)0)
#endif

#endif /* MN1AA_DEBUG_LOG_H */
