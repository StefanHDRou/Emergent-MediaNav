/*
 * color_convert.c - Fused YCbCr -> RGB565 for MIPS Au1320
 *
 * CRITICAL OPTIMIZATION: This converts directly to RGB565 without
 * creating an intermediate RGB888 buffer. This saves:
 * - 3 bytes/pixel write (RGB888) = 384 bytes per 8x8 block
 * - One complete pass over the data
 *
 * YCbCr to RGB conversion (ITU-R BT.601):
 *   R = Y + 1.402 * (Cr - 128)
 *   G = Y - 0.344136 * (Cb - 128) - 0.714136 * (Cr - 128)
 *   B = Y + 1.772 * (Cb - 128)
 *
 * Fixed-point (shift by 16):
 *   R = Y + ((91881 * Cr) >> 16) - 179
 *   G = Y - ((22554 * Cb + 46802 * Cr) >> 16) + 135
 *   B = Y + ((116130 * Cb) >> 16) - 226
 *
 * Then pack to RGB565:
 *   pixel = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)
 *
 * FURTHER OPTIMIZATION: Use lookup tables for Cb/Cr contributions.
 * Pre-compute Cr->R_delta, Cb/Cr->G_delta, Cb->B_delta for all 256 values.
 * This turns 3 multiplications per pixel into 3 table lookups.
 * Tables: 3 * 256 * 2 = 1536 bytes (fits in D-cache).
 */

#include "color_convert.h"

/* Lookup tables for Cb/Cr contributions (pre-computed at init) */
static int16_t s_crToR[256];    /* 1.402 * (Cr - 128) */
static int16_t s_cbToG[256];    /* -0.344136 * (Cb - 128) */
static int16_t s_crToG[256];    /* -0.714136 * (Cr - 128) */
static int16_t s_cbToB[256];    /* 1.772 * (Cb - 128) */

static int s_tablesBuilt = 0;

/* Build lookup tables (called once at startup) */
static void build_color_tables(void)
{
    int i;

    if (s_tablesBuilt)
        return;

    for (i = 0; i < 256; i++) {
        int c = i - 128;
        s_crToR[i] = (int16_t)((91881 * c + 32768) >> 16);
        s_cbToG[i] = (int16_t)((-22554 * c + 32768) >> 16);
        s_crToG[i] = (int16_t)((-46802 * c + 32768) >> 16);
        s_cbToB[i] = (int16_t)((116130 * c + 32768) >> 16);
    }

    s_tablesBuilt = 1;
}

/* Clamp to [0, 255] - branchless version using bit manipulation */
static __inline uint8_t clamp255(int val)
{
    /* If val < 0:   val & ~0xFF != 0, and val is negative -> return 0
     * If val > 255: val & ~0xFF != 0                     -> return 255
     * Otherwise:    return val */
    if ((unsigned int)val > 255) {
        return (uint8_t)((-val) >> 31); /* 0 if negative, 255 if > 255 */
        /* Simplified: */
    }
    return (uint8_t)val;
}

/* Pack RGB to RGB565 */
static __inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void ycbcr_to_rgb565_mcu(
    int16_t         mcuBlocks[6][64],
    uint16_t*       pOutput,
    uint32_t        dwStride,
    int             hSamp,
    int             vSamp)
{
    build_color_tables();

    if (hSamp == 2 && vSamp == 2) {
        /*
         * 4:2:0 subsampling: Most common case.
         *
         * MCU = 16x16 pixels
         * Y: 4 blocks in 2x2 arrangement (each 8x8)
         *   [Block0 | Block1]
         *   [Block2 | Block3]
         * Cb: 1 block covering full 16x16 (each sample covers 2x2 pixels)
         * Cr: 1 block covering full 16x16
         *
         * For each Cb/Cr sample, we compute R/G/B deltas once
         * and apply to 4 Y pixels (2x2 upsampling of chroma).
         */

        int16_t* yTL = mcuBlocks[0];  /* Y top-left 8x8 */
        int16_t* yTR = mcuBlocks[1];  /* Y top-right 8x8 */
        int16_t* yBL = mcuBlocks[2];  /* Y bottom-left 8x8 */
        int16_t* yBR = mcuBlocks[3];  /* Y bottom-right 8x8 */
        int16_t* cb  = mcuBlocks[4];  /* Cb 8x8 */
        int16_t* cr  = mcuBlocks[5];  /* Cr 8x8 */

        int cy, cx; /* Chroma block coordinates (0-7) */

        for (cy = 0; cy < 8; cy++) {
            /* Output row pointers for this chroma row (covers 2 luma rows) */
            uint16_t* row0 = pOutput + (cy * 2) * dwStride;
            uint16_t* row1 = row0 + dwStride;

            for (cx = 0; cx < 8; cx++) {
                /* Get Cb/Cr values for this 2x2 pixel block */
                int cbVal = cb[cy * 8 + cx];
                int crVal = cr[cy * 8 + cx];

                /* Look up color deltas (pre-computed, no multiply!) */
                int rDelta = s_crToR[crVal & 0xFF];
                int gDelta = s_cbToG[cbVal & 0xFF] + s_crToG[crVal & 0xFF];
                int bDelta = s_cbToB[cbVal & 0xFF];

                /* Determine which Y block and offset */
                int ly0 = cy;       /* Row within Y block */
                int lx0 = cx;       /* Column within Y block */

                int16_t* yBlock;
                int yOff;
                int r, g, b, y;

                /* Pixel (2*cx, 2*cy) - top-left of 2x2 */
                if (cx < 4 && cy < 4) yBlock = yTL;
                else if (cx >= 4 && cy < 4) yBlock = yTR;
                else if (cx < 4 && cy >= 4) yBlock = yBL;
                else yBlock = yBR;

                /* Top-left Y block coordinates */
                {
                    int bx = (cx < 4) ? cx : (cx - 4);
                    int by = (cy < 4) ? cy : (cy - 4);

                    /* Pixel (0,0) of 2x2 group */
                    y = yBlock[by * 8 + bx];
                    r = y + rDelta; g = y + gDelta; b = y + bDelta;
                    row0[cx * 2] = pack_rgb565(
                        clamp255(r), clamp255(g), clamp255(b));

                    /* Pixel (1,0) */
                    /* For 2x2 upsampling, adjacent Y pixel shares chroma */
                    if (bx + 1 < 8) {
                        y = yBlock[by * 8 + bx + 1];
                    }
                    r = y + rDelta; g = y + gDelta; b = y + bDelta;
                    row0[cx * 2 + 1] = pack_rgb565(
                        clamp255(r), clamp255(g), clamp255(b));

                    /* Pixel (0,1) */
                    if (by + 1 < 8) {
                        y = yBlock[(by + 1) * 8 + bx];
                    }
                    r = y + rDelta; g = y + gDelta; b = y + bDelta;
                    row1[cx * 2] = pack_rgb565(
                        clamp255(r), clamp255(g), clamp255(b));

                    /* Pixel (1,1) */
                    if (bx + 1 < 8 && by + 1 < 8) {
                        y = yBlock[(by + 1) * 8 + bx + 1];
                    }
                    r = y + rDelta; g = y + gDelta; b = y + bDelta;
                    row1[cx * 2 + 1] = pack_rgb565(
                        clamp255(r), clamp255(g), clamp255(b));
                }
            }
        }

    } else if (hSamp == 1 && vSamp == 1) {
        /*
         * 4:4:4 (no subsampling) - Simple case.
         * MCU = 8x8 pixels, 1:1 chroma.
         */
        int16_t* yBlock = mcuBlocks[0];
        int16_t* cb     = mcuBlocks[4];
        int16_t* cr     = mcuBlocks[5];
        int py, px;

        for (py = 0; py < 8; py++) {
            uint16_t* row = pOutput + py * dwStride;
            for (px = 0; px < 8; px++) {
                int idx = py * 8 + px;
                int y = yBlock[idx];
                int cbVal = cb[idx];
                int crVal = cr[idx];

                int r = y + s_crToR[crVal & 0xFF];
                int g = y + s_cbToG[cbVal & 0xFF] + s_crToG[crVal & 0xFF];
                int b = y + s_cbToB[cbVal & 0xFF];

                row[px] = pack_rgb565(
                    clamp255(r), clamp255(g), clamp255(b));
            }
        }

    } else {
        /* 4:2:2 or other - simplified handling */
        /* For 4:2:2: hSamp=2, vSamp=1, MCU=16x8 */
        int16_t* cb = mcuBlocks[4];
        int16_t* cr = mcuBlocks[5];
        int py, px;

        for (py = 0; py < 8; py++) {
            uint16_t* row = pOutput + py * dwStride;
            for (px = 0; px < 8 * hSamp; px++) {
                int yBlockIdx = px / 8;
                int yLocalX = px % 8;
                int y = mcuBlocks[yBlockIdx][py * 8 + yLocalX];

                int chromaX = px / hSamp;
                int chromaY = py / vSamp;
                int chromaIdx = chromaY * 8 + chromaX;
                int cbVal = cb[chromaIdx & 63];
                int crVal = cr[chromaIdx & 63];

                int r = y + s_crToR[crVal & 0xFF];
                int g = y + s_cbToG[cbVal & 0xFF] + s_crToG[crVal & 0xFF];
                int b = y + s_cbToB[cbVal & 0xFF];

                row[px] = pack_rgb565(
                    clamp255(r), clamp255(g), clamp255(b));
            }
        }
    }
}
