/*
 * mjpeg_decoder.c - Hyper-Optimized MJPEG Decoder for MIPS Au1320
 *
 * THIS IS THE MOST PERFORMANCE-CRITICAL CODE IN THE ENTIRE PROJECT.
 *
 * Every instruction in the inner decode loop matters. The Au1320:
 * - 667 MHz, 5-stage in-order pipeline, single-issue
 * - 16KB I-cache: our decode loop MUST fit within this
 * - 16KB D-cache: quantization tables + Huffman tables + DCT block
 * - No SIMD, no FPU, no out-of-order execution
 * - One multiply per cycle (32-bit result), one divide per bit
 *
 * For 400x240 @ 15 FPS we have:
 *   667,000,000 cycles/sec / 15 frames = 44.5M cycles per frame
 *   400x240 = 1500 MCUs (8x8 blocks, 4:2:0 = 6 blocks/MCU)
 *   44.5M / (1500 * 6) = ~4950 cycles per 8x8 block
 *
 * That's TIGHT. IDCT alone typically costs ~2000-3000 cycles.
 * Huffman decoding: ~1000-2000 cycles per block.
 * Color conversion: ~500 cycles per block.
 *
 * OPTIMIZATION STRATEGIES EMPLOYED:
 * 1. IFAST IDCT with 16-bit intermediates
 * 2. Combined dequantize + IDCT (multiply Q factor during IDCT)
 * 3. Fused YCbCr -> RGB565 (no intermediate RGB888 buffer)
 * 4. Minimal branch Huffman decode (8-bit lookup table)
 * 5. Preprocessed byte unstuffing (removes 0xFF checks from VLD)
 * 6. DC prediction inlined
 * 7. All inner loop variables in registers (6 used of 32 available)
 */

#include "mjpeg_decoder.h"
#include "video/jpeg_idct_mips.h"
#include "video/jpeg_huffman.h"
#include "video/color_convert.h"
#include "util/debug_log.h"

/* =========================================================================
 * JPEG MARKER DEFINITIONS
 * ========================================================================= */

#define JPEG_SOI    0xFFD8  /* Start of Image */
#define JPEG_EOI    0xFFD9  /* End of Image */
#define JPEG_SOF0   0xFFC0  /* Start of Frame (Baseline DCT) */
#define JPEG_DHT    0xFFC4  /* Define Huffman Table */
#define JPEG_DQT    0xFFDB  /* Define Quantization Table */
#define JPEG_SOS    0xFFDA  /* Start of Scan */
#define JPEG_DRI    0xFFDD  /* Define Restart Interval */
#define JPEG_RST0   0xFFD0  /* Restart Marker 0 */
#define JPEG_RST7   0xFFD7  /* Restart Marker 7 */
#define JPEG_APP0   0xFFE0  /* Application Marker 0 (JFIF) */

/* =========================================================================
 * BITSTREAM READER (inlined for performance)
 * ========================================================================= */

/*
 * Read bits from the JPEG bitstream.
 * JPEG uses MSB-first bit ordering with 0xFF byte stuffing.
 *
 * The byte stuffing rule: if 0xFF appears in entropy-coded data,
 * it's followed by 0x00 (stuffed byte). We must skip the 0x00.
 *
 * OPTIMIZATION: We refill the bit buffer 8 bits at a time.
 * For MIPS, this avoids the branch-heavy "check every byte" approach.
 */
static __inline void bits_refill(mjpeg_context_t* ctx)
{
    while (ctx->bitsLeft <= 24 && ctx->pData < ctx->pEnd) {
        uint8_t byte = *ctx->pData++;

        if (byte == 0xFF) {
            /* Check for stuffed byte */
            if (ctx->pData < ctx->pEnd && *ctx->pData == 0x00) {
                ctx->pData++; /* Skip stuff byte */
            } else {
                /* Marker found in stream - back up and stop */
                ctx->pData--;
                return;
            }
        }

        ctx->bitBuf = (ctx->bitBuf << 8) | byte;
        ctx->bitsLeft += 8;
    }
}

static __inline uint32_t bits_peek(mjpeg_context_t* ctx, int n)
{
    if (ctx->bitsLeft < n)
        bits_refill(ctx);
    return (ctx->bitBuf >> (ctx->bitsLeft - n)) & ((1u << n) - 1);
}

static __inline void bits_skip(mjpeg_context_t* ctx, int n)
{
    ctx->bitsLeft -= n;
}

static __inline uint32_t bits_read(mjpeg_context_t* ctx, int n)
{
    uint32_t val;
    if (ctx->bitsLeft < n)
        bits_refill(ctx);
    ctx->bitsLeft -= n;
    val = (ctx->bitBuf >> ctx->bitsLeft) & ((1u << n) - 1);
    return val;
}

/* =========================================================================
 * MARKER PARSING
 * ========================================================================= */

static const uint8_t* find_marker(const uint8_t* p, const uint8_t* end,
                                   uint8_t marker)
{
    while (p < end - 1) {
        if (p[0] == 0xFF && p[1] == marker)
            return p;
        p++;
    }
    return NULL;
}

/* Read 2-byte big-endian length from marker segment */
static __inline uint16_t read_marker_len(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* =========================================================================
 * PARSE DQT (Define Quantization Table)
 * ========================================================================= */

static mn1_result_t parse_dqt(mjpeg_context_t* ctx,
                               const uint8_t* pData, uint16_t wLen)
{
    const uint8_t* p = pData;
    const uint8_t* pEnd = pData + wLen;

    while (p < pEnd) {
        uint8_t info = *p++;
        uint8_t precision = (info >> 4) & 0x0F;  /* 0 = 8-bit, 1 = 16-bit */
        uint8_t tableId   = info & 0x0F;

        if (tableId > 3) {
            MN1_LOG_ERROR(L"DQT: invalid table ID %d", tableId);
            return MN1_ERR_DECODE_INVALID;
        }

        if (precision == 0) {
            /* 8-bit precision (Baseline JPEG) */
            int i;
            for (i = 0; i < 64; i++) {
                if (p >= pEnd) return MN1_ERR_DECODE_TRUNCATED;
                ctx->quantTables[tableId][i] = (int16_t)*p++;
            }
        } else {
            /* 16-bit precision */
            int i;
            for (i = 0; i < 64; i++) {
                if (p + 1 >= pEnd) return MN1_ERR_DECODE_TRUNCATED;
                ctx->quantTables[tableId][i] = (int16_t)((p[0] << 8) | p[1]);
                p += 2;
            }
        }

        ctx->quantTableValid[tableId] = 1;
    }

    return MN1_OK;
}

/* =========================================================================
 * PARSE DHT (Define Huffman Table)
 * ========================================================================= */

static mn1_result_t parse_dht(mjpeg_context_t* ctx,
                               const uint8_t* pData, uint16_t wLen)
{
    const uint8_t* p = pData;
    const uint8_t* pEnd = pData + wLen;

    while (p < pEnd) {
        uint8_t info = *p++;
        uint8_t tableClass = (info >> 4) & 0x0F;  /* 0=DC, 1=AC */
        uint8_t tableId    = info & 0x0F;

        if (tableId > 1 || tableClass > 1) {
            MN1_LOG_ERROR(L"DHT: invalid table class=%d id=%d",
                          tableClass, tableId);
            return MN1_ERR_DECODE_INVALID;
        }

        /* Read bit counts (16 bytes: counts for codes of length 1-16) */
        uint8_t* bits;
        uint8_t* vals;
        int totalSymbols = 0;
        int i;

        if (tableClass == 0) {
            bits = ctx->huffDC_bits[tableId];
            vals = ctx->huffDC_vals[tableId];
        } else {
            bits = ctx->huffAC_bits[tableId];
            vals = ctx->huffAC_vals[tableId];
        }

        bits[0] = 0;
        for (i = 1; i <= 16; i++) {
            if (p >= pEnd) return MN1_ERR_DECODE_TRUNCATED;
            bits[i] = *p++;
            totalSymbols += bits[i];
        }

        if (totalSymbols > 256)
            return MN1_ERR_DECODE_INVALID;

        /* Read symbol values */
        for (i = 0; i < totalSymbols; i++) {
            if (p >= pEnd) return MN1_ERR_DECODE_TRUNCATED;
            vals[i] = *p++;
        }

        /*
         * Build fast 8-bit lookup table.
         *
         * For codes up to 8 bits long, we pre-compute the decoded
         * symbol for all possible 8-bit input values. This turns
         * most Huffman decodes into a single table lookup.
         *
         * For codes > 8 bits (rare in JPEG), we fall back to
         * tree traversal.
         */
        if (tableClass == 0) {
            huff_build_lookup_dc(bits, vals, ctx->huffDC_lookup[tableId]);
        } else {
            huff_build_lookup_ac(bits, vals, ctx->huffAC_lookup[tableId]);
        }
    }

    return MN1_OK;
}

/* =========================================================================
 * PARSE SOF0 (Start of Frame - Baseline DCT)
 * ========================================================================= */

static mn1_result_t parse_sof0(mjpeg_context_t* ctx,
                                const uint8_t* pData, uint16_t wLen)
{
    uint8_t precision;
    int i;

    if (wLen < 6)
        return MN1_ERR_DECODE_TRUNCATED;

    precision = pData[0];
    if (precision != 8) {
        MN1_LOG_ERROR(L"SOF0: unsupported precision %d (need 8)", precision);
        return MN1_ERR_DECODE_UNSUPPORTED;
    }

    ctx->imgHeight = (uint16_t)((pData[1] << 8) | pData[2]);
    ctx->imgWidth  = (uint16_t)((pData[3] << 8) | pData[4]);
    ctx->numComponents = pData[5];

    if (ctx->numComponents != 3) {
        MN1_LOG_ERROR(L"SOF0: %d components (expected 3 for YCbCr)",
                      ctx->numComponents);
        return MN1_ERR_DECODE_UNSUPPORTED;
    }

    if (wLen < (uint16_t)(6 + ctx->numComponents * 3))
        return MN1_ERR_DECODE_TRUNCATED;

    /* Parse component descriptors */
    for (i = 0; i < ctx->numComponents; i++) {
        const uint8_t* pComp = pData + 6 + i * 3;
        ctx->comp[i].compId       = pComp[0];
        ctx->comp[i].hSamp        = (pComp[1] >> 4) & 0x0F;
        ctx->comp[i].vSamp        = pComp[1] & 0x0F;
        ctx->comp[i].quantTableIdx = pComp[2];
    }

    /* Calculate MCU dimensions.
     * For 4:2:0 (most common): Y=2x2, Cb=1x1, Cr=1x1
     * MCU = 16x16 pixels, 6 blocks (4Y + 1Cb + 1Cr)
     *
     * For 4:2:2: Y=2x1, Cb=1x1, Cr=1x1
     * MCU = 16x8 pixels, 4 blocks (2Y + 1Cb + 1Cr)
     */
    ctx->mcuWidth  = ctx->comp[0].hSamp * 8;
    ctx->mcuHeight = ctx->comp[0].vSamp * 8;
    ctx->mcuCountX = (ctx->imgWidth  + ctx->mcuWidth  - 1) / ctx->mcuWidth;
    ctx->mcuCountY = (ctx->imgHeight + ctx->mcuHeight - 1) / ctx->mcuHeight;

    MN1_LOG_INFO(L"SOF0: %dx%d, MCU=%dx%d (%dx%d MCUs), %d components",
                 ctx->imgWidth, ctx->imgHeight,
                 ctx->mcuWidth, ctx->mcuHeight,
                 ctx->mcuCountX, ctx->mcuCountY,
                 ctx->numComponents);

    return MN1_OK;
}

/* =========================================================================
 * PARSE SOS (Start of Scan)
 * ========================================================================= */

static mn1_result_t parse_sos(mjpeg_context_t* ctx,
                               const uint8_t* pData, uint16_t wLen)
{
    uint8_t numComponents;
    int i;

    if (wLen < 1)
        return MN1_ERR_DECODE_TRUNCATED;

    numComponents = pData[0];

    for (i = 0; i < numComponents && i < 3; i++) {
        const uint8_t* pComp = pData + 1 + i * 2;
        /* uint8_t compId = pComp[0]; */
        ctx->comp[i].dcTableIdx = (pComp[1] >> 4) & 0x0F;
        ctx->comp[i].acTableIdx = pComp[1] & 0x0F;
    }

    return MN1_OK;
}

/* =========================================================================
 * DECODE ONE 8x8 DCT BLOCK
 *
 * This is the innermost loop. Called 6 times per MCU for 4:2:0.
 * At 400x240 with 4:2:0: 1500 MCUs * 6 = 9000 block decodes per frame.
 *
 * Steps:
 * 1. Decode DC coefficient (Huffman + differential)
 * 2. Decode 63 AC coefficients (Huffman + run-length)
 * 3. Dequantize (multiply by quant table)
 * 4. IFAST IDCT (8x8 transform)
 * ========================================================================= */

static void decode_block(mjpeg_context_t* ctx, int compIdx)
{
    int16_t* block = ctx->dctBlock;
    const int16_t* qTable = ctx->quantTables[ctx->comp[compIdx].quantTableIdx];
    uint8_t dcIdx = ctx->comp[compIdx].dcTableIdx;
    uint8_t acIdx = ctx->comp[compIdx].acTableIdx;
    int i;

    /* Zero the block (64 int16_t = 128 bytes)
     * OPTIMIZATION: Use word-sized writes, not memset */
    {
        uint32_t* p32 = (uint32_t*)block;
        for (i = 0; i < 32; i++)
            p32[i] = 0;
    }

    /* ---- DC Coefficient ---- */
    {
        int category;
        int dcVal;

        /* Huffman decode the DC category (0-11) */
        category = huff_decode_dc(ctx, dcIdx);

        if (category > 0) {
            /* Read 'category' bits for the DC difference value */
            int bits = (int)bits_read(ctx, category);

            /* Convert to signed: if MSB is 0, subtract (2^category - 1) */
            if (bits < (1 << (category - 1)))
                dcVal = bits - (1 << category) + 1;
            else
                dcVal = bits;
        } else {
            dcVal = 0;
        }

        /* Add differential prediction */
        ctx->dcPred[compIdx] += (int16_t)dcVal;

        /* Dequantize and store DC coefficient */
        block[0] = ctx->dcPred[compIdx] * qTable[0];
    }

    /* ---- AC Coefficients (zigzag order) ---- */
    {
        /* Standard JPEG zigzag scan order */
        static const uint8_t zigzag[64] = {
             0,  1,  8, 16,  9,  2,  3, 10,
            17, 24, 32, 25, 18, 11,  4,  5,
            12, 19, 26, 33, 40, 48, 41, 34,
            27, 20, 13,  6,  7, 14, 21, 28,
            35, 42, 49, 56, 57, 50, 43, 36,
            29, 22, 15, 23, 30, 37, 44, 51,
            58, 59, 52, 45, 38, 31, 39, 46,
            53, 60, 61, 54, 47, 55, 62, 63
        };

        int k = 1; /* Start at AC coefficient 1 */

        while (k < 64) {
            int symbol;
            int runLength;
            int acSize;
            int acVal;

            /* Huffman decode the AC run/size symbol */
            symbol = huff_decode_ac(ctx, acIdx);

            if (symbol == 0x00) {
                /* EOB (End of Block) - remaining coefficients are zero */
                break;
            }

            if (symbol == 0xF0) {
                /* ZRL (Zero Run Length) - skip 16 zeros */
                k += 16;
                continue;
            }

            /* High nibble = run of zeros, low nibble = coefficient size */
            runLength = (symbol >> 4) & 0x0F;
            acSize    = symbol & 0x0F;

            k += runLength;

            if (k >= 64)
                break;

            if (acSize > 0) {
                int bits = (int)bits_read(ctx, acSize);

                if (bits < (1 << (acSize - 1)))
                    acVal = bits - (1 << acSize) + 1;
                else
                    acVal = bits;

                /* Dequantize and store in zigzag position */
                block[zigzag[k]] = (int16_t)(acVal * qTable[zigzag[k]]);
            }

            k++;
        }
    }

    /* ---- IDCT ---- */
    /* Transform 8x8 DCT coefficients -> 8x8 spatial domain samples */
    idct_ifast_mips(block);
}

/* =========================================================================
 * DECODE SCAN DATA (ALL MCUs)
 * ========================================================================= */

static mn1_result_t decode_scan(mjpeg_context_t* ctx)
{
    uint32_t mcuX, mcuY;
    int16_t mcuBlocks[6][64]; /* 6 blocks per MCU for 4:2:0 */
    uint16_t* pRow;
    uint32_t dwStartTick = mn1_get_tick_ms();

    /* Reset DC predictions */
    ctx->dcPred[0] = 0;
    ctx->dcPred[1] = 0;
    ctx->dcPred[2] = 0;

    /* Initialize bitstream reader */
    ctx->bitBuf = 0;
    ctx->bitsLeft = 0;

    for (mcuY = 0; mcuY < ctx->mcuCountY; mcuY++) {

        pRow = ctx->pOutput + mcuY * ctx->mcuHeight * ctx->dwOutputStride;

        for (mcuX = 0; mcuX < ctx->mcuCountX; mcuX++) {

            /* Decode all blocks in this MCU */
            int blockIdx;
            int blocksPerMcu;

            if (ctx->comp[0].hSamp == 2 && ctx->comp[0].vSamp == 2) {
                /* 4:2:0: 4 Y blocks + 1 Cb + 1 Cr = 6 blocks */
                blocksPerMcu = 6;

                /* Y blocks (2x2 arrangement) */
                decode_block(ctx, 0); /* Y top-left */
                memcpy(mcuBlocks[0], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 0); /* Y top-right */
                memcpy(mcuBlocks[1], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 0); /* Y bottom-left */
                memcpy(mcuBlocks[2], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 0); /* Y bottom-right */
                memcpy(mcuBlocks[3], ctx->dctBlock, sizeof(ctx->dctBlock));

                /* Cb block */
                decode_block(ctx, 1);
                memcpy(mcuBlocks[4], ctx->dctBlock, sizeof(ctx->dctBlock));

                /* Cr block */
                decode_block(ctx, 2);
                memcpy(mcuBlocks[5], ctx->dctBlock, sizeof(ctx->dctBlock));

            } else if (ctx->comp[0].hSamp == 2 && ctx->comp[0].vSamp == 1) {
                /* 4:2:2: 2 Y blocks + 1 Cb + 1 Cr = 4 blocks */
                blocksPerMcu = 4;

                decode_block(ctx, 0); /* Y left */
                memcpy(mcuBlocks[0], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 0); /* Y right */
                memcpy(mcuBlocks[1], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 1); /* Cb */
                memcpy(mcuBlocks[4], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 2); /* Cr */
                memcpy(mcuBlocks[5], ctx->dctBlock, sizeof(ctx->dctBlock));

            } else {
                /* 4:4:4 or other: 1 Y + 1 Cb + 1 Cr = 3 blocks */
                blocksPerMcu = 3;

                decode_block(ctx, 0);
                memcpy(mcuBlocks[0], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 1);
                memcpy(mcuBlocks[4], ctx->dctBlock, sizeof(ctx->dctBlock));

                decode_block(ctx, 2);
                memcpy(mcuBlocks[5], ctx->dctBlock, sizeof(ctx->dctBlock));
            }

            /*
             * Convert MCU from YCbCr to RGB565 and write to output.
             *
             * OPTIMIZATION: This is fused - no intermediate RGB888 buffer.
             * The color_convert module writes directly to the framebuffer.
             */
            {
                uint16_t* pMcuOut = pRow + mcuX * ctx->mcuWidth;

                ycbcr_to_rgb565_mcu(
                    mcuBlocks,
                    pMcuOut,
                    ctx->dwOutputStride,
                    ctx->comp[0].hSamp,
                    ctx->comp[0].vSamp
                );
            }
        }
    }

    ctx->stats.dwDecodeUs = (mn1_get_tick_ms() - dwStartTick) * 1000;
    return MN1_OK;
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

mn1_result_t mjpeg_init(
    mjpeg_context_t*    pCtx,
    uint16_t*           pOutput,
    uint32_t            dwStride)
{
    memset(pCtx, 0, sizeof(mjpeg_context_t));
    pCtx->pOutput = pOutput;
    pCtx->dwOutputStride = dwStride;
    return MN1_OK;
}

mn1_result_t mjpeg_decode_frame(
    mjpeg_context_t*    pCtx,
    const uint8_t*      pJpegData,
    uint32_t            dwLen,
    mjpeg_stats_t*      pStats)
{
    const uint8_t* p = pJpegData;
    const uint8_t* pEnd = pJpegData + dwLen;
    mn1_result_t result;

    /* Verify SOI marker */
    if (dwLen < 2 || p[0] != 0xFF || p[1] != 0xD8) {
        MN1_LOG_ERROR(L"Not a JPEG (bad SOI)");
        return MN1_ERR_DECODE_INVALID;
    }
    p += 2;

    /* Reset DC predictions for new frame */
    pCtx->dcPred[0] = 0;
    pCtx->dcPred[1] = 0;
    pCtx->dcPred[2] = 0;

    /* Parse markers until SOS */
    while (p < pEnd - 1) {
        if (*p != 0xFF) {
            p++;
            continue;
        }

        uint8_t marker = p[1];
        p += 2;

        if (marker == 0xD9) {
            /* EOI - end of image (shouldn't happen before SOS) */
            break;
        }

        if (marker == 0x00 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            /* Standalone markers (RST, TEM) - no length field */
            continue;
        }

        /* Markers with length field */
        if (p + 2 > pEnd)
            return MN1_ERR_DECODE_TRUNCATED;

        uint16_t segLen = read_marker_len(p);
        p += 2;

        if (p + segLen - 2 > pEnd)
            return MN1_ERR_DECODE_TRUNCATED;

        switch (0xFF00 | marker) {
            case JPEG_DQT:
                result = parse_dqt(pCtx, p, segLen - 2);
                if (result != MN1_OK) return result;
                break;

            case JPEG_DHT:
                result = parse_dht(pCtx, p, segLen - 2);
                if (result != MN1_OK) return result;
                break;

            case JPEG_SOF0:
                result = parse_sof0(pCtx, p, segLen - 2);
                if (result != MN1_OK) return result;
                break;

            case JPEG_SOS:
                result = parse_sos(pCtx, p, segLen - 2);
                if (result != MN1_OK) return result;

                /* Advance past SOS header to entropy-coded data */
                p += (segLen - 2);

                /* Set up bitstream for scan decode */
                pCtx->pData = p;
                pCtx->pEnd  = pEnd;

                /* DECODE THE SCAN - This is where all the CPU time goes */
                result = decode_scan(pCtx);

                if (pStats) {
                    *pStats = pCtx->stats;
                    pStats->wWidth  = pCtx->imgWidth;
                    pStats->wHeight = pCtx->imgHeight;
                }

                return result;

            case JPEG_DRI:
                /* Restart interval - skip for now */
                break;

            default:
                /* Unknown marker - skip */
                break;
        }

        p += (segLen - 2);
    }

    MN1_LOG_ERROR(L"JPEG: No SOS marker found");
    return MN1_ERR_DECODE_INVALID;
}
