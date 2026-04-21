/*
 * ring_buffer_portable.c - Portable ring buffer (uses malloc instead of LocalAlloc)
 */
#ifdef PORTABLE_BUILD

#include "types_portable.h"
#include "util/ring_buffer.h"

mn1_result_t mn1_ring_init(mn1_ring_buffer_t* pRing, uint32_t dwSize)
{
    if (dwSize == 0 || (dwSize & (dwSize - 1)) != 0)
        return MN1_ERR_GENERIC;

    pRing->pBuffer = (uint8_t*)malloc(dwSize);
    if (!pRing->pBuffer)
        return MN1_ERR_OUT_OF_MEMORY;

    pRing->dwSize = dwSize;
    pRing->dwHead = 0;
    pRing->dwTail = 0;
    return MN1_OK;
}

void mn1_ring_free(mn1_ring_buffer_t* pRing)
{
    if (pRing->pBuffer) {
        free(pRing->pBuffer);
        pRing->pBuffer = NULL;
    }
}

uint32_t mn1_ring_readable(const mn1_ring_buffer_t* pRing)
{
    return (pRing->dwHead - pRing->dwTail) & (pRing->dwSize - 1);
}

uint32_t mn1_ring_writable(const mn1_ring_buffer_t* pRing)
{
    return (pRing->dwSize - 1) - mn1_ring_readable(pRing);
}

uint32_t mn1_ring_write(mn1_ring_buffer_t* pRing,
                         const uint8_t* pData, uint32_t dwLen)
{
    uint32_t dwAvail = mn1_ring_writable(pRing);
    uint32_t dwMask = pRing->dwSize - 1;
    uint32_t dwHead, i;

    if (dwLen > dwAvail) dwLen = dwAvail;
    dwHead = pRing->dwHead;
    for (i = 0; i < dwLen; i++)
        pRing->pBuffer[(dwHead + i) & dwMask] = pData[i];
    pRing->dwHead = (dwHead + dwLen) & dwMask;
    return dwLen;
}

uint32_t mn1_ring_read(mn1_ring_buffer_t* pRing,
                        uint8_t* pData, uint32_t dwLen)
{
    uint32_t dwAvail = mn1_ring_readable(pRing);
    uint32_t dwMask = pRing->dwSize - 1;
    uint32_t dwTail, i;

    if (dwLen > dwAvail) dwLen = dwAvail;
    dwTail = pRing->dwTail;
    for (i = 0; i < dwLen; i++)
        pData[i] = pRing->pBuffer[(dwTail + i) & dwMask];
    pRing->dwTail = (dwTail + dwLen) & dwMask;
    return dwLen;
}

uint32_t mn1_ring_peek(const mn1_ring_buffer_t* pRing,
                        uint8_t* pData, uint32_t dwLen)
{
    uint32_t dwAvail = mn1_ring_readable(pRing);
    uint32_t dwMask = pRing->dwSize - 1;
    uint32_t dwTail = pRing->dwTail;
    uint32_t i;
    if (dwLen > dwAvail) dwLen = dwAvail;
    for (i = 0; i < dwLen; i++)
        pData[i] = pRing->pBuffer[(dwTail + i) & dwMask];
    return dwLen;
}

void mn1_ring_skip(mn1_ring_buffer_t* pRing, uint32_t dwLen)
{
    uint32_t dwAvail = mn1_ring_readable(pRing);
    if (dwLen > dwAvail) dwLen = dwAvail;
    pRing->dwTail = (pRing->dwTail + dwLen) & (pRing->dwSize - 1);
}

void mn1_ring_reset(mn1_ring_buffer_t* pRing)
{
    pRing->dwHead = 0;
    pRing->dwTail = 0;
}

#endif /* PORTABLE_BUILD */
