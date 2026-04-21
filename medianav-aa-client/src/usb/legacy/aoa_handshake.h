/*
 * aoa_handshake.h - Android Open Accessory Protocol Handshake
 *
 * Implements the AOA protocol to switch an Android device into
 * Accessory mode over USB, establishing bulk IN/OUT communication.
 *
 * AOA Handshake Sequence (from USB Host perspective):
 *
 *   1. Detect USB device connection
 *   2. Send GET_PROTOCOL (vendor request 51) -> read protocol version
 *   3. Send identification strings (vendor request 52, indices 0-5)
 *   4. Send START (vendor request 53) -> device re-enumerates
 *   5. Wait for re-enumeration with VID=0x18D1, PID=0x2D00/0x2D01
 *   6. Claim interface 0, discover bulk IN/OUT endpoints
 *   7. Begin bulk data transfer
 *
 * Reference: https://source.android.com/docs/core/interaction/accessories/aoa
 */

#ifndef MN1AA_AOA_HANDSHAKE_H
#define MN1AA_AOA_HANDSHAKE_H

#include "config.h"
#include "types.h"
#include "usb/usb_host.h"

/* AOA protocol version returned by the phone */
typedef enum {
    AOA_PROTOCOL_NONE = 0,
    AOA_PROTOCOL_V1   = 1,     /* AOAv1: basic accessory mode */
    AOA_PROTOCOL_V2   = 2,     /* AOAv2: adds HID/audio support */
} aoa_protocol_version_t;

/* AOA handshake state machine */
typedef enum {
    AOA_STATE_IDLE = 0,
    AOA_STATE_CHECKING_PROTOCOL,
    AOA_STATE_SENDING_STRINGS,
    AOA_STATE_SWITCHING,
    AOA_STATE_WAITING_REENUMERATE,
    AOA_STATE_CONNECTED,
    AOA_STATE_FAILED,
} aoa_state_t;

/* AOA handshake context */
typedef struct {
    aoa_state_t             state;
    aoa_protocol_version_t  protocolVersion;
    mn1_usb_conn_t*         pConn;
} aoa_context_t;

/*
 * Perform the complete AOA handshake.
 *
 * This function executes the full AOA sequence:
 * 1. Check protocol support
 * 2. Send identity strings (from config.h MN1_AOA_* macros)
 * 3. Switch to accessory mode
 * 4. Wait for re-enumeration
 * 5. Open bulk endpoints
 *
 * On success, pConn->hBulkIn and pConn->hBulkOut are ready for use.
 *
 * @param pConn  USB connection (must have hUSBDevice opened)
 * @return MN1_OK on success, error code on failure
 *
 * IMPORTANT: After this function returns MN1_OK, the phone will
 * have a new USB device identity (VID:PID = 18D1:2D00). The original
 * device handle is invalidated and a new one is stored in pConn.
 */
mn1_result_t mn1_aoa_handshake(mn1_usb_conn_t* pConn);

/*
 * Check AOA protocol support only (step 2 of handshake).
 * Useful for probing before committing to full handshake.
 *
 * @param pConn     USB connection
 * @param pVersion  Output: protocol version (0 = not supported)
 * @return MN1_OK if device responds, MN1_ERR_USB_AOA_UNSUPPORTED if not
 */
mn1_result_t mn1_aoa_check_protocol(
    mn1_usb_conn_t*          pConn,
    aoa_protocol_version_t*  pVersion
);

#endif /* MN1AA_AOA_HANDSHAKE_H */
