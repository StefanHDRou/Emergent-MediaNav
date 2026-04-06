/*
 * tls_minimal.h / tls_minimal.c - Minimal TLS 1.2 Wrapper
 * Stub for standard AAP path (AAP requires TLS-encrypted channels).
 */
#ifndef MN1AA_TLS_MINIMAL_H
#define MN1AA_TLS_MINIMAL_H
#include "types.h"

mn1_result_t mn1_tls_init(void);
void mn1_tls_shutdown(void);

#endif
