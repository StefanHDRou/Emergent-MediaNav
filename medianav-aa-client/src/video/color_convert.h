/*
 * color_convert.h - YCbCr to RGB565 Fused Conversion
 */

#ifndef MN1AA_COLOR_CONVERT_H
#define MN1AA_COLOR_CONVERT_H

#include "types.h"

/*
 * Convert one MCU from YCbCr (decoded IDCT blocks) to RGB565.
 *
 * Writes directly to the framebuffer, avoiding intermediate RGB888.
 *
 * @param mcuBlocks     Array of 6 decoded 8x8 blocks (4Y + Cb + Cr for 4:2:0)
 *                      Block layout: [Y_TL][Y_TR][Y_BL][Y_BR][Cb][Cr]
 * @param pOutput       Destination RGB565 buffer (framebuffer row)
 * @param dwStride      Output stride in pixels (not bytes)
 * @param hSamp         Horizontal sampling factor of Y (1 or 2)
 * @param vSamp         Vertical sampling factor of Y (1 or 2)
 *
 * RGB565 format: RRRRRGGGGGGBBBBB (5-6-5 bits)
 *   R = (val >> 11) & 0x1F
 *   G = (val >> 5)  & 0x3F
 *   B = val & 0x1F
 */
void ycbcr_to_rgb565_mcu(
    int16_t         mcuBlocks[6][64],
    uint16_t*       pOutput,
    uint32_t        dwStride,
    int             hSamp,
    int             vSamp
);

#endif /* MN1AA_COLOR_CONVERT_H */
