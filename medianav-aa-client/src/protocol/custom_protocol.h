/*
 * custom_protocol.h - Custom MJPEG Streaming Protocol (Path B)
 *
 * This is the "viable path" protocol for the MediaNav MN1.
 * Used between the custom Android companion app and the MN1 client.
 *
 * Wire Format:
 *
 *   [Header 8 bytes][Payload N bytes]
 *
 *   Header:
 *     Offset 0: uint16_t magic   = 0xAA55
 *     Offset 2: uint8_t  type    = FRAME_VIDEO / FRAME_TOUCH / FRAME_CONFIG
 *     Offset 3: uint8_t  flags   = reserved
 *     Offset 4: uint32_t length  = payload size in bytes (little-endian)
 *
 *   VIDEO payload: Raw JPEG data (SOI to EOI)
 *   TOUCH payload: mn1_touch_event_t structure
 *   CONFIG payload: Stream configuration parameters
 *
 * Flow:
 *   Phone -> MN1:  VIDEO frames (continuous stream)
 *   MN1 -> Phone:  TOUCH events + CONFIG requests
 */

#ifndef MN1AA_CUSTOM_PROTOCOL_H
#define MN1AA_CUSTOM_PROTOCOL_H

#include "config.h"
#include "types.h"

/* Configuration message sent from MN1 -> Phone */
typedef struct {
    uint16_t    wWidth;         /* Requested capture width */
    uint16_t    wHeight;        /* Requested capture height */
    uint8_t     bQuality;       /* JPEG quality (1-100) */
    uint8_t     bMaxFps;        /* Maximum FPS requested */
    uint8_t     bReserved[2];
} mn1_config_msg_t;

/*
 * Initialize the custom protocol layer.
 * Must be called after AOA handshake establishes bulk pipes.
 *
 * @param pConn  Active USB connection with bulk endpoints
 * @return MN1_OK on success
 */
mn1_result_t mn1_custom_init(mn1_usb_conn_t* pConn);

/*
 * Send initial configuration to the companion app.
 * Tells the phone what resolution and quality to encode at.
 *
 * @param pConn    USB connection
 * @param pConfig  Configuration to send
 * @return MN1_OK on success
 */
mn1_result_t mn1_custom_send_config(
    mn1_usb_conn_t*         pConn,
    const mn1_config_msg_t* pConfig
);

/*
 * Read the next video frame from USB.
 *
 * This function:
 * 1. Reads the 8-byte header
 * 2. Validates magic + type
 * 3. Reads the JPEG payload into the provided buffer
 *
 * @param pConn        USB connection
 * @param pBuffer      Buffer for JPEG data (must be >= dwMaxBytes)
 * @param dwMaxBytes   Buffer size
 * @param pdwActual    Output: actual JPEG data size
 * @return MN1_OK on success, MN1_ERR_DECODE_INVALID if not a video frame
 */
mn1_result_t mn1_custom_read_video_frame(
    mn1_usb_conn_t* pConn,
    uint8_t*        pBuffer,
    uint32_t        dwMaxBytes,
    uint32_t*       pdwActual
);

/*
 * Send a touch event to the phone.
 *
 * @param pConn   USB connection
 * @param pEvent  Touch event data
 * @return MN1_OK on success
 */
mn1_result_t mn1_custom_send_touch(
    mn1_usb_conn_t*          pConn,
    const mn1_touch_event_t* pEvent
);

#endif /* MN1AA_CUSTOM_PROTOCOL_H */
