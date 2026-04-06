/*
 * jpeg_idct_mips.h - MIPS-Optimized IFAST IDCT
 */

#ifndef MN1AA_JPEG_IDCT_MIPS_H
#define MN1AA_JPEG_IDCT_MIPS_H

#include "types.h"

/*
 * In-place IFAST (Loeffler) 8x8 IDCT.
 *
 * Input:  64 dequantized DCT coefficients (int16_t[64], row-major)
 * Output: 64 spatial-domain samples (int16_t[64], clamped to [0,255])
 *
 * This uses the AAN (Arai, Agui, Nakajima) scaled DCT algorithm
 * with fixed-point arithmetic. The "fast" variant uses fewer
 * multiplications at the cost of reduced precision.
 *
 * Precision: +-1 LSB vs. reference IDCT (acceptable for video display)
 *
 * Cycle count estimate on Au1320:
 *   ~2000-2500 cycles per 8x8 block (with 16-bit intermediates)
 */
void idct_ifast_mips(int16_t block[64]);

#endif /* MN1AA_JPEG_IDCT_MIPS_H */
