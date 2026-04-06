/*
 * jpeg_huffman.h - Optimized Huffman Decoder for JPEG
 */

#ifndef MN1AA_JPEG_HUFFMAN_H
#define MN1AA_JPEG_HUFFMAN_H

#include "types.h"

/* Forward declaration - mjpeg_context_t is defined in mjpeg_decoder.h */
struct mjpeg_context_t_tag;

/*
 * Build 8-bit fast lookup table for DC Huffman codes.
 * For codes <= 8 bits, the lookup returns the decoded symbol directly.
 * For codes > 8 bits, returns 0xFF to indicate slow path needed.
 *
 * Lookup entry format: [4-bit code_length | 4-bit symbol]
 */
void huff_build_lookup_dc(
    const uint8_t* bits,    /* Bit count array [17] */
    const uint8_t* vals,    /* Symbol values */
    uint8_t* lookup         /* Output: 256-entry lookup table */
);

/*
 * Build 8-bit fast lookup table for AC Huffman codes.
 * Same principle as DC but with 16-bit entries for (run,size) pairs.
 *
 * Lookup entry format: [8-bit code_length | 8-bit symbol]
 */
void huff_build_lookup_ac(
    const uint8_t* bits,
    const uint8_t* vals,
    uint16_t* lookup        /* Output: 256-entry lookup table */
);

/*
 * Decode one DC Huffman symbol from the bitstream.
 * Uses fast 8-bit lookup with slow-path fallback.
 *
 * @return DC category (0-11)
 *
 * NOTE: These functions access ctx->bitBuf/bitsLeft directly.
 * The function signature takes void* to avoid circular header deps.
 */
int huff_decode_dc(void* ctx, int tableIdx);

/*
 * Decode one AC Huffman symbol from the bitstream.
 *
 * @return AC run/size byte (high nibble=run, low nibble=size)
 *         0x00 = EOB, 0xF0 = ZRL
 */
int huff_decode_ac(void* ctx, int tableIdx);

#endif /* MN1AA_JPEG_HUFFMAN_H */
