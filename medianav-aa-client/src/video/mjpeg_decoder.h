/*
 * mjpeg_decoder.h - MJPEG Decoder Interface
 *
 * Hyper-optimized JPEG decoder for MIPS Au1320 @ 667MHz.
 *
 * Design decisions for maximum performance on this CPU:
 *
 * 1. IFAST IDCT (not ISLOW) - trades accuracy for ~40% speed gain
 * 2. 16-bit fixed-point throughout - fits in MIPS 32-bit registers
 * 3. No floating point - Au1320 has no FPU worth using
 * 4. Fused YCbCr->RGB565 conversion - skip intermediate RGB888
 * 5. Minimal memory allocation - single scratch buffer, no malloc in decode loop
 * 6. Byte unstuffing preprocessed - removes 0xFF marker checks from inner loop
 * 7. Supports only Baseline JPEG (8-bit, Huffman, sequential DCT)
 *    - No progressive JPEG
 *    - No arithmetic coding
 *    - No 12-bit samples
 */

#ifndef MN1AA_MJPEG_DECODER_H
#define MN1AA_MJPEG_DECODER_H

#include "config.h"
#include "types.h"

/* Maximum supported image dimensions */
#define MJPEG_MAX_WIDTH         800
#define MJPEG_MAX_HEIGHT        480

/* Decode result statistics */
typedef struct {
    uint32_t    dwDecodeUs;     /* Total decode time (microseconds) */
    uint32_t    dwIdctUs;       /* IDCT time */
    uint32_t    dwHuffmanUs;    /* Huffman decoding time */
    uint32_t    dwColorUs;      /* Color conversion time */
    uint16_t    wWidth;         /* Decoded image width */
    uint16_t    wHeight;        /* Decoded image height */
} mjpeg_stats_t;

/* Decoder context (pre-allocated, reused across frames) */
typedef struct {
    /* Quantization tables (up to 4, 64 entries each, 16-bit) */
    int16_t     quantTables[4][64];
    uint8_t     quantTableValid[4];

    /* Huffman tables (DC + AC, up to 2 tables each) */
    /* Fast lookup: huffVal[code] where code < 256 */
    uint8_t     huffDC_lookup[2][256];  /* DC Huffman fast lookup */
    uint8_t     huffDC_bits[2][17];     /* Bit counts */
    uint8_t     huffDC_vals[2][256];    /* Values */
    uint16_t    huffAC_lookup[2][256];  /* AC Huffman fast lookup */
    uint8_t     huffAC_bits[2][17];
    uint8_t     huffAC_vals[2][256];

    /* Component info (Y, Cb, Cr) */
    uint8_t     numComponents;
    struct {
        uint8_t compId;
        uint8_t hSamp;          /* Horizontal sampling factor */
        uint8_t vSamp;          /* Vertical sampling factor */
        uint8_t quantTableIdx;
        uint8_t dcTableIdx;
        uint8_t acTableIdx;
    } comp[3];

    /* MCU dimensions */
    uint16_t    mcuWidth;       /* MCU width in pixels */
    uint16_t    mcuHeight;      /* MCU height in pixels */
    uint16_t    mcuCountX;      /* MCUs per row */
    uint16_t    mcuCountY;      /* MCUs per column */

    /* Image dimensions */
    uint16_t    imgWidth;
    uint16_t    imgHeight;

    /* DC prediction state (reset at each restart marker) */
    int16_t     dcPred[3];

    /* Bitstream reader state */
    const uint8_t*  pData;      /* Current position in JPEG data */
    const uint8_t*  pEnd;       /* End of JPEG data */
    uint32_t    bitBuf;         /* Bit accumulator */
    int         bitsLeft;       /* Bits remaining in accumulator */

    /* Scratch buffers (statically sized, no dynamic alloc) */
    int16_t     dctBlock[64];   /* Current DCT block being decoded */

    /* Output buffer */
    uint16_t*   pOutput;        /* RGB565 output buffer */
    uint32_t    dwOutputStride; /* Output stride in pixels */

    /* Performance counters */
    mjpeg_stats_t stats;

} mjpeg_context_t;

/*
 * Initialize decoder context. Call once at startup.
 *
 * @param pCtx       Decoder context to initialize
 * @param pOutput    Pre-allocated RGB565 output buffer
 * @param dwStride   Output stride in pixels (width of output buffer row)
 * @return MN1_OK on success
 */
mn1_result_t mjpeg_init(
    mjpeg_context_t*    pCtx,
    uint16_t*           pOutput,
    uint32_t            dwStride
);

/*
 * Decode a single JPEG frame.
 *
 * This is the main entry point. Decodes a complete JPEG image
 * (from SOI to EOI markers) into the RGB565 output buffer.
 *
 * @param pCtx       Decoder context
 * @param pJpegData  JPEG data (starting with 0xFF 0xD8)
 * @param dwLen      JPEG data length
 * @param pStats     Output: decode statistics (optional, can be NULL)
 * @return MN1_OK on success
 *
 * PERFORMANCE TARGET on Au1320 @ 667MHz:
 *   400x240 Q50: ~50ms per frame (20 FPS)
 *   800x480 Q50: ~100ms per frame (10 FPS)
 */
mn1_result_t mjpeg_decode_frame(
    mjpeg_context_t*    pCtx,
    const uint8_t*      pJpegData,
    uint32_t            dwLen,
    mjpeg_stats_t*      pStats
);

#endif /* MN1AA_MJPEG_DECODER_H */
