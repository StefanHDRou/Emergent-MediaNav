/*
 * aap_service.c - AAP Service Layer Stub
 * Full implementation needed for standard AAP path (Path A).
 */
#include "protocol/aap_service.h"
#include "protocol/pb_lite.h"
#include "util/debug_log.h"

mn1_result_t mn1_aap_service_init(mn1_usb_conn_t* pConn) {
    (void)pConn;
    MN1_LOG_INFO(L"AAP service layer initialized (stub)");
    return MN1_OK;
}

mn1_result_t mn1_aap_send_version_request(mn1_usb_conn_t* pConn) {
    /* VERSION_REQUEST (msg type 1) with protocol version 1.6 */
    uint8_t payload[16];
    int n = 0;
    n += pb_encode_uint32(payload + n, 1, MN1_AAP_VERSION_MAJOR);
    n += pb_encode_uint32(payload + n, 2, MN1_AAP_VERSION_MINOR);
    return mn1_aap_write_control(pConn, 1, payload, (uint32_t)n);
}

mn1_result_t mn1_aap_send_service_discovery(mn1_usb_conn_t* pConn) {
    (void)pConn;
    MN1_LOG_INFO(L"AAP service discovery (stub - implement for Path A)");
    return MN1_OK;
}
