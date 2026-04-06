/*
 * debug_log.c - Debug Logging Implementation
 *
 * Writes to a file on the Storage Card or to serial port.
 * Compiled out entirely in release builds (MN1_LOG_LEVEL == 0).
 */

#include "util/debug_log.h"
#include <stdarg.h>

#if MN1_LOG_LEVEL >= 1

static HANDLE s_hLogFile = INVALID_HANDLE_VALUE;
static const WCHAR* s_levelNames[] = { L"", L"ERR", L"WRN", L"INF" };

void mn1_log_init(void)
{
#if MN1_LOG_TARGET == MN1_LOG_TARGET_FILE
    s_hLogFile = CreateFile(
        MN1_LOG_FILE_PATH,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (s_hLogFile != INVALID_HANDLE_VALUE) {
        /* Write UTF-16 BOM */
        DWORD dwWritten;
        uint16_t bom = 0xFEFF;
        WriteFile(s_hLogFile, &bom, 2, &dwWritten, NULL);
    }
#elif MN1_LOG_TARGET == MN1_LOG_TARGET_SERIAL
    /* Serial output goes through OutputDebugString on WinCE */
#endif
}

void mn1_log_shutdown(void)
{
    if (s_hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(s_hLogFile);
        s_hLogFile = INVALID_HANDLE_VALUE;
    }
}

void mn1_log_write(int level, const WCHAR* fmt, ...)
{
    WCHAR buf[512];
    va_list args;
    int len;
    DWORD dwTick = GetTickCount();

    /* Format: [TICK] [LEVEL] message */
    len = wsprintf(buf, L"[%08u] [%s] ", dwTick, s_levelNames[level]);

    va_start(args, fmt);
    len += wvsprintf(buf + len, fmt, args);
    va_end(args);

    /* Add newline */
    if (len < 510) {
        buf[len++] = L'\r';
        buf[len++] = L'\n';
    }
    buf[len] = 0;

#if MN1_LOG_TARGET == MN1_LOG_TARGET_FILE
    if (s_hLogFile != INVALID_HANDLE_VALUE) {
        DWORD dwWritten;
        WriteFile(s_hLogFile, buf, len * sizeof(WCHAR), &dwWritten, NULL);
    }
#elif MN1_LOG_TARGET == MN1_LOG_TARGET_SERIAL
    OutputDebugString(buf);
#endif

    /* Also output via DEBUGMSG for debug builds */
#ifdef DEBUG
    OutputDebugString(buf);
#endif
}

#else /* MN1_LOG_LEVEL == 0 */

void mn1_log_init(void) { }
void mn1_log_shutdown(void) { }

#endif
