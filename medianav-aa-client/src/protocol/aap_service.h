/*
 * aap_service.h / aap_service.c - AAP Service Discovery & Channel Management
 * (Stub - needed only for standard AAP path)
 */
#ifndef MN1AA_AAP_SERVICE_H
#define MN1AA_AAP_SERVICE_H
#include "config.h"
#include "types.h"
#include "protocol/aap_framing.h"

/* Placeholder for standard AAP service layer */
mn1_result_t mn1_aap_service_init(mn1_usb_conn_t* pConn);
mn1_result_t mn1_aap_send_version_request(mn1_usb_conn_t* pConn);
mn1_result_t mn1_aap_send_service_discovery(mn1_usb_conn_t* pConn);

#endif
