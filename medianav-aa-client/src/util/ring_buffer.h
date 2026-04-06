/*
 * ring_buffer.h - Lock-Free Single-Producer Single-Consumer Ring Buffer
 */

#ifndef MN1AA_RING_BUFFER_H
#define MN1AA_RING_BUFFER_H

#include "types.h"

/*
 * Initialize ring buffer.
 * @param pRing  Ring buffer structure
 * @param dwSize Buffer size (MUST be power of 2)
 * @return MN1_OK on success
 */
mn1_result_t mn1_ring_init(mn1_ring_buffer_t* pRing, uint32_t dwSize);

/* Free ring buffer memory */
void mn1_ring_free(mn1_ring_buffer_t* pRing);

/* Get number of bytes available for reading */
uint32_t mn1_ring_readable(const mn1_ring_buffer_t* pRing);

/* Get number of bytes available for writing */
uint32_t mn1_ring_writable(const mn1_ring_buffer_t* pRing);

/*
 * Write data to ring buffer (producer side - USB thread).
 * @return Bytes actually written (may be less if buffer full)
 */
uint32_t mn1_ring_write(mn1_ring_buffer_t* pRing,
                         const uint8_t* pData, uint32_t dwLen);

/*
 * Read data from ring buffer (consumer side - decode thread).
 * @return Bytes actually read (may be less if buffer empty)
 */
uint32_t mn1_ring_read(mn1_ring_buffer_t* pRing,
                        uint8_t* pData, uint32_t dwLen);

/*
 * Peek at data without consuming it.
 * @return Bytes available to peek
 */
uint32_t mn1_ring_peek(const mn1_ring_buffer_t* pRing,
                        uint8_t* pData, uint32_t dwLen);

/* Discard (skip) bytes from the read side */
void mn1_ring_skip(mn1_ring_buffer_t* pRing, uint32_t dwLen);

/* Reset to empty state */
void mn1_ring_reset(mn1_ring_buffer_t* pRing);

#endif /* MN1AA_RING_BUFFER_H */
