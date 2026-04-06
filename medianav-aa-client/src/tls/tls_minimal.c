/*
 * tls_minimal.c - TLS Stub
 *
 * Standard AAP requires TLS 1.2 for all communication after version exchange.
 * For the Companion MJPEG path, TLS is NOT used (custom protocol is unencrypted).
 *
 * To implement for Path A: Port a minimal TLS 1.2 library (e.g., BearSSL or
 * mbedTLS) to MIPS WinCE. BearSSL is recommended (~100KB code, no dynamic alloc).
 *
 * WARNING: TLS handshake alone may take 500ms+ on Au1320 due to RSA/ECDSA ops.
 */
#include "tls/tls_minimal.h"
#include "util/debug_log.h"

mn1_result_t mn1_tls_init(void) {
    MN1_LOG_INFO(L"TLS stub initialized (not needed for Companion MJPEG path)");
    return MN1_OK;
}

void mn1_tls_shutdown(void) { }
