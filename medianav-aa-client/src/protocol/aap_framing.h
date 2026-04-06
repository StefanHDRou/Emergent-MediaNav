/*
 * aap_framing.h - Android Auto Protocol Frame Layer
 *
 * Handles AAP frame encoding/decoding for the standard protocol path.
 *
 * AAP Frame Format:
 *   [Channel 1B] [Flags 1B] [FrameLen 2B BE] {TotalLen 4B BE if first} [Payload]
 *
 * Channel IDs:
 *   0 = Control channel (service discovery, version, auth, ping)
 *   1+ = Dynamically allocated service channels
 *
 * Flags:
 *   0x01 = First fragment
 *   0x02 = Last fragment
 *   0x03 = Unfragmented (first + last)
 *
 * Max frame payload: 16384 bytes (0x4000)
 */

#ifndef MN1AA_AAP_FRAMING_H
#define MN1AA_AAP_FRAMING_H

#include "config.h"
#include "types.h"

/* Parsed AAP frame */
typedef struct {
    uint8_t     bChannel;
    uint8_t     bFlags;
    uint16_t    wFrameLen;
    uint32_t    dwTotalLen;     /* Only valid if bFlags & FIRST */
    uint8_t*    pPayload;
    mn1_bool_t  bIsFirst;
    mn1_bool_t  bIsLast;
} mn1_aap_frame_t;

/*
 * Read one AAP frame from the USB connection.
 * Handles the variable-length header (4 or 8 bytes).
 *
 * @param pConn     USB connection
 * @param pFrame    Output: parsed frame (pPayload points into pBuffer)
 * @param pBuffer   Scratch buffer for frame data
 * @param dwBufSize Buffer size (must be >= MN1_AAP_MAX_FRAME_SIZE + 8)
 * @return MN1_OK on success
 */
mn1_result_t mn1_aap_read_frame(
    mn1_usb_conn_t*     pConn,
    mn1_aap_frame_t*    pFrame,
    uint8_t*            pBuffer,
    uint32_t            dwBufSize
);

/*
 * Write one AAP frame to the USB connection.
 *
 * @param pConn      USB connection
 * @param bChannel   Channel ID
 * @param pPayload   Payload data
 * @param dwLen      Payload length
 * @return MN1_OK on success
 *
 * If dwLen > MN1_AAP_MAX_FRAME_SIZE, automatically fragments.
 */
mn1_result_t mn1_aap_write_frame(
    mn1_usb_conn_t* pConn,
    uint8_t         bChannel,
    const uint8_t*  pPayload,
    uint32_t        dwLen
);

/*
 * Write a control channel message (channel 0).
 * Adds the 2-byte message type prefix before the protobuf payload.
 *
 * @param pConn      USB connection
 * @param wMsgType   Control message type (ControlMessageType enum)
 * @param pPayload   Protobuf-encoded message body
 * @param dwLen      Body length
 * @return MN1_OK on success
 */
mn1_result_t mn1_aap_write_control(
    mn1_usb_conn_t* pConn,
    uint16_t        wMsgType,
    const uint8_t*  pPayload,
    uint32_t        dwLen
);

#endif /* MN1AA_AAP_FRAMING_H */
