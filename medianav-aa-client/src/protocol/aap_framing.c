/*
 * aap_framing.c - AAP Frame Layer Implementation
 *
 * Encodes/decodes the AAP transport framing over USB bulk endpoints.
 * All multi-byte integers in AAP headers are big-endian.
 */

#include "aap_framing.h"
#include "util/debug_log.h"

mn1_result_t mn1_aap_read_frame(
    mn1_usb_conn_t*     pConn,
    mn1_aap_frame_t*    pFrame,
    uint8_t*            pBuffer,
    uint32_t            dwBufSize)
{
    uint32_t dwActual;
    mn1_result_t result;
    uint32_t dwHeaderSize;
    uint16_t wFrameLen;

    /* Read minimum header (4 bytes) */
    result = mn1_usb_bulk_read(pConn, pBuffer, 4, &dwActual, 5000);
    if (result != MN1_OK)
        return result;
    if (dwActual < 4)
        return MN1_ERR_DECODE_TRUNCATED;

    /* Parse header fields */
    pFrame->bChannel  = pBuffer[0];
    pFrame->bFlags    = pBuffer[1];
    pFrame->wFrameLen = mn1_read_be16(&pBuffer[2]);
    pFrame->bIsFirst  = (pFrame->bFlags & MN1_AAP_FLAG_FIRST) ? TRUE : FALSE;
    pFrame->bIsLast   = (pFrame->bFlags & MN1_AAP_FLAG_LAST) ? TRUE : FALSE;

    wFrameLen = pFrame->wFrameLen;

    /* If first fragment, read the 4-byte total length field */
    if (pFrame->bIsFirst) {
        result = mn1_usb_bulk_read(pConn, pBuffer + 4, 4, &dwActual, 2000);
        if (result != MN1_OK)
            return result;
        pFrame->dwTotalLen = mn1_read_be32(&pBuffer[4]);
        dwHeaderSize = MN1_AAP_HEADER_SIZE_FIRST;

        /* Adjust frame payload length (total length field is part of payload
         * area in the first frame, so actual payload = wFrameLen - 4) */
        if (wFrameLen >= 4)
            wFrameLen -= 4;
    } else {
        pFrame->dwTotalLen = 0;
        dwHeaderSize = MN1_AAP_HEADER_SIZE;
    }

    /* Validate frame length */
    if (wFrameLen > MN1_AAP_MAX_FRAME_SIZE) {
        MN1_LOG_ERROR(L"AAP frame too large: %u bytes", wFrameLen);
        return MN1_ERR_DECODE_INVALID;
    }

    if (wFrameLen > dwBufSize - dwHeaderSize) {
        MN1_LOG_ERROR(L"AAP frame exceeds buffer: %u > %u",
                      wFrameLen, dwBufSize - dwHeaderSize);
        return MN1_ERR_OUT_OF_MEMORY;
    }

    /* Read payload */
    if (wFrameLen > 0) {
        uint32_t dwRemaining = wFrameLen;
        uint32_t dwOffset = 0;
        uint8_t* pPayloadBuf = pBuffer + dwHeaderSize;

        while (dwRemaining > 0) {
            result = mn1_usb_bulk_read(pConn, pPayloadBuf + dwOffset,
                                        dwRemaining, &dwActual, 2000);
            if (result != MN1_OK)
                return result;
            if (dwActual == 0)
                return MN1_ERR_DECODE_TRUNCATED;
            dwOffset += dwActual;
            dwRemaining -= dwActual;
        }

        pFrame->pPayload = pPayloadBuf;
    } else {
        pFrame->pPayload = NULL;
    }

    pFrame->wFrameLen = wFrameLen;
    return MN1_OK;
}

mn1_result_t mn1_aap_write_frame(
    mn1_usb_conn_t* pConn,
    uint8_t         bChannel,
    const uint8_t*  pPayload,
    uint32_t        dwLen)
{
    uint8_t header[MN1_AAP_HEADER_SIZE_FIRST];
    uint32_t dwActual;
    mn1_result_t result;

    if (dwLen <= MN1_AAP_MAX_FRAME_SIZE) {
        /* Single unfragmented frame */
        header[0] = bChannel;
        header[1] = MN1_AAP_FLAG_UNFRAG;  /* First + Last */
        mn1_write_be16(&header[2], (uint16_t)(dwLen + 4));
        mn1_write_be32(&header[4], dwLen);

        /* Write header */
        result = mn1_usb_bulk_write(pConn, header,
                                     MN1_AAP_HEADER_SIZE_FIRST,
                                     &dwActual, 1000);
        if (result != MN1_OK)
            return result;

        /* Write payload */
        if (dwLen > 0) {
            result = mn1_usb_bulk_write(pConn, pPayload, dwLen,
                                         &dwActual, 1000);
        }
        return result;
    }

    /*
     * Fragmented write: split into multiple frames of max 0x4000 each.
     * First frame has FIRST flag + total length.
     * Middle frames have CONTINUATION (0x00).
     * Last frame has LAST flag.
     */
    {
        uint32_t dwOffset = 0;
        uint32_t dwRemaining = dwLen;
        mn1_bool_t bFirst = TRUE;

        while (dwRemaining > 0) {
            uint32_t dwChunk = dwRemaining;
            uint8_t bFlags;

            if (dwChunk > MN1_AAP_MAX_FRAME_SIZE)
                dwChunk = MN1_AAP_MAX_FRAME_SIZE;

            if (bFirst && (dwRemaining <= MN1_AAP_MAX_FRAME_SIZE)) {
                bFlags = MN1_AAP_FLAG_UNFRAG;
            } else if (bFirst) {
                bFlags = MN1_AAP_FLAG_FIRST;
            } else if (dwRemaining <= MN1_AAP_MAX_FRAME_SIZE) {
                bFlags = MN1_AAP_FLAG_LAST;
            } else {
                bFlags = 0; /* Continuation */
            }

            header[0] = bChannel;
            header[1] = bFlags;

            if (bFirst) {
                /* Chunk size includes the 4-byte total length field */
                mn1_write_be16(&header[2], (uint16_t)(dwChunk + 4));
                mn1_write_be32(&header[4], dwLen);
                result = mn1_usb_bulk_write(pConn, header,
                                             MN1_AAP_HEADER_SIZE_FIRST,
                                             &dwActual, 1000);
                bFirst = FALSE;
            } else {
                mn1_write_be16(&header[2], (uint16_t)dwChunk);
                result = mn1_usb_bulk_write(pConn, header,
                                             MN1_AAP_HEADER_SIZE,
                                             &dwActual, 1000);
            }

            if (result != MN1_OK)
                return result;

            result = mn1_usb_bulk_write(pConn, pPayload + dwOffset,
                                         dwChunk, &dwActual, 1000);
            if (result != MN1_OK)
                return result;

            dwOffset += dwChunk;
            dwRemaining -= dwChunk;
        }
    }

    return MN1_OK;
}

mn1_result_t mn1_aap_write_control(
    mn1_usb_conn_t* pConn,
    uint16_t        wMsgType,
    const uint8_t*  pPayload,
    uint32_t        dwLen)
{
    /*
     * Control messages on channel 0 have a 2-byte message type
     * prefix before the protobuf body.
     */
    uint8_t msgBuf[MN1_AAP_MAX_FRAME_SIZE];
    uint32_t dwTotalLen = 2 + dwLen;

    if (dwTotalLen > sizeof(msgBuf))
        return MN1_ERR_OUT_OF_MEMORY;

    mn1_write_be16(msgBuf, wMsgType);
    if (pPayload && dwLen > 0)
        memcpy(msgBuf + 2, pPayload, dwLen);

    return mn1_aap_write_frame(pConn, 0, msgBuf, dwTotalLen);
}
