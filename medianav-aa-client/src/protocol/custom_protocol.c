/*
 * custom_protocol.c - Custom MJPEG Streaming Protocol Implementation
 *
 * Lightweight protocol optimized for minimal overhead on the MN1.
 * Header parsing is inlined and branch-free where possible.
 */

#include "custom_protocol.h"
#include "util/debug_log.h"

/* Pre-built config frame header (reused for all config sends) */
static uint8_t s_configHeader[MN1_CUSTOM_HEADER_SIZE];
static uint8_t s_touchHeader[MN1_CUSTOM_HEADER_SIZE];

mn1_result_t mn1_custom_init(mn1_usb_conn_t* pConn)
{
    (void)pConn;

    /* Pre-build static headers to avoid per-send construction */

    /* Config header */
    s_configHeader[0] = (uint8_t)(MN1_CUSTOM_MAGIC & 0xFF);
    s_configHeader[1] = (uint8_t)(MN1_CUSTOM_MAGIC >> 8);
    s_configHeader[2] = MN1_CUSTOM_FRAME_CONFIG;
    s_configHeader[3] = 0;
    mn1_write_le32(&s_configHeader[4], sizeof(mn1_config_msg_t));

    /* Touch header (length filled per-send) */
    s_touchHeader[0] = (uint8_t)(MN1_CUSTOM_MAGIC & 0xFF);
    s_touchHeader[1] = (uint8_t)(MN1_CUSTOM_MAGIC >> 8);
    s_touchHeader[2] = MN1_CUSTOM_FRAME_TOUCH;
    s_touchHeader[3] = 0;
    mn1_write_le32(&s_touchHeader[4], sizeof(mn1_touch_event_t));

    MN1_LOG_INFO(L"Custom protocol initialized");
    return MN1_OK;
}

mn1_result_t mn1_custom_send_config(
    mn1_usb_conn_t*         pConn,
    const mn1_config_msg_t* pConfig)
{
    uint8_t txBuf[MN1_CUSTOM_HEADER_SIZE + sizeof(mn1_config_msg_t)];
    uint32_t dwActual;

    /* Assemble packet: header + config payload */
    memcpy(txBuf, s_configHeader, MN1_CUSTOM_HEADER_SIZE);
    memcpy(txBuf + MN1_CUSTOM_HEADER_SIZE, pConfig, sizeof(mn1_config_msg_t));

    MN1_LOG_INFO(L"Sending config: %dx%d Q%d %dfps",
                 pConfig->wWidth, pConfig->wHeight,
                 pConfig->bQuality, pConfig->bMaxFps);

    return mn1_usb_bulk_write(
        pConn,
        txBuf,
        sizeof(txBuf),
        &dwActual,
        1000
    );
}

mn1_result_t mn1_custom_read_video_frame(
    mn1_usb_conn_t* pConn,
    uint8_t*        pBuffer,
    uint32_t        dwMaxBytes,
    uint32_t*       pdwActual)
{
    uint8_t header[MN1_CUSTOM_HEADER_SIZE];
    uint32_t dwActual;
    uint32_t dwPayloadLen;
    uint16_t wMagic;
    mn1_result_t result;

    /*
     * Read header (8 bytes).
     *
     * OPTIMIZATION: On USB 2.0 HS, the minimum bulk transfer is
     * a 512-byte packet. If we request exactly 8 bytes, the USB
     * controller still transfers a full packet. So we could read
     * header + start of payload in one transfer. But for clarity
     * and robustness, we read header separately.
     *
     * TODO: For production, read larger chunks and parse inline.
     */
    result = mn1_usb_bulk_read(pConn, header, MN1_CUSTOM_HEADER_SIZE,
                                &dwActual, 5000);
    if (result != MN1_OK)
        return result;

    if (dwActual < MN1_CUSTOM_HEADER_SIZE)
        return MN1_ERR_DECODE_TRUNCATED;

    /* Validate magic (little-endian: 0x55 first, 0xAA second) */
    wMagic = (uint16_t)(header[0] | (header[1] << 8));
    if (wMagic != MN1_CUSTOM_MAGIC) {
        MN1_LOG_ERROR(L"Bad magic: 0x%04X (expected 0x%04X)",
                      wMagic, MN1_CUSTOM_MAGIC);
        return MN1_ERR_DECODE_INVALID;
    }

    /* Check frame type */
    if (header[2] != MN1_CUSTOM_FRAME_VIDEO) {
        /* Not a video frame - might be a touch ACK or other message */
        MN1_LOG_WARN(L"Non-video frame type: 0x%02X", header[2]);
        return MN1_ERR_DECODE_UNSUPPORTED;
    }

    /* Read payload length (little-endian) */
    dwPayloadLen = mn1_read_le32(&header[4]);

    if (dwPayloadLen > dwMaxBytes) {
        MN1_LOG_ERROR(L"Frame too large: %u > %u", dwPayloadLen, dwMaxBytes);
        return MN1_ERR_OUT_OF_MEMORY;
    }

    if (dwPayloadLen == 0) {
        *pdwActual = 0;
        return MN1_OK;
    }

    /*
     * Read JPEG payload.
     *
     * USB bulk transfers may be split across multiple packets.
     * We must read until we have dwPayloadLen bytes.
     */
    {
        uint32_t dwRemaining = dwPayloadLen;
        uint32_t dwOffset = 0;

        while (dwRemaining > 0) {
            uint32_t dwChunkRead = 0;

            result = mn1_usb_bulk_read(
                pConn,
                pBuffer + dwOffset,
                dwRemaining,
                &dwChunkRead,
                2000
            );

            if (result != MN1_OK)
                return result;

            if (dwChunkRead == 0) {
                MN1_LOG_ERROR(L"Zero-length read during frame payload");
                return MN1_ERR_USB_TRANSFER;
            }

            dwOffset += dwChunkRead;
            dwRemaining -= dwChunkRead;
        }
    }

    *pdwActual = dwPayloadLen;
    return MN1_OK;
}

mn1_result_t mn1_custom_send_touch(
    mn1_usb_conn_t*          pConn,
    const mn1_touch_event_t* pEvent)
{
    uint8_t txBuf[MN1_CUSTOM_HEADER_SIZE + sizeof(mn1_touch_event_t)];
    uint32_t dwActual;

    memcpy(txBuf, s_touchHeader, MN1_CUSTOM_HEADER_SIZE);
    memcpy(txBuf + MN1_CUSTOM_HEADER_SIZE, pEvent, sizeof(mn1_touch_event_t));

    return mn1_usb_bulk_write(
        pConn,
        txBuf,
        sizeof(txBuf),
        &dwActual,
        100  /* Touch events need low latency */
    );
}
