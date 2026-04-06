# 03 - Performance Analysis & Memory Budget

## The Brutal Truth

The Alchemy Au1320 at 667 MHz is a **1990s-era MIPS core** running at a
clock speed that was mid-range in 2009 when the chip was announced.
Here's what we're working with:

| Spec | Value | Impact |
|------|-------|--------|
| Clock | 667 MHz | ~667M simple instructions/sec |
| Pipeline | 5-stage in-order | No out-of-order execution, no speculation |
| Issue width | Single-issue | ONE instruction per clock maximum |
| I-Cache | 16 KB (4-way) | Hot loop MUST fit in 16KB |
| D-Cache | 16 KB (4-way) | Working set MUST fit in 16KB |
| Multiply | 1 cycle (32-bit result) | Good for fixed-point math |
| Divide | ~32 cycles | Avoid at all costs in inner loops |
| FPU | Negligible | All math must be integer/fixed-point |
| SIMD | None confirmed | No vectorization possible |
| Memory BW | DDR2-667, 16-bit bus | ~1.3 GB/s peak, ~600 MB/s sustained |

---

## JPEG Decode Cycle Budget

### Target: 400x240 @ 15 FPS (Half Resolution)

```
Available cycles per frame:
  667,000,000 cycles/sec / 15 FPS = 44,466,666 cycles/frame

Image size: 400 x 240 = 96,000 pixels

For 4:2:0 subsampling:
  MCU = 16x16 pixels
  Blocks per MCU = 6 (4Y + 1Cb + 1Cr)
  MCUs = (400/16) * (240/16) = 25 * 15 = 375 MCUs
  Total blocks = 375 * 6 = 2,250 blocks

Cycles per block budget:
  44,466,666 / 2,250 = 19,763 cycles per block

This is GENEROUS. Here's the actual cost breakdown:
```

### Per-Block Cycle Estimates

| Stage | Estimated Cycles | % of Budget | Notes |
|-------|-----------------|-------------|-------|
| Huffman decode (VLD) | ~1,500 | 7.6% | 8-bit lookup fast path |
| Dequantize | ~200 | 1.0% | 64 multiplies (fused with IDCT) |
| IFAST IDCT | ~2,500 | 12.6% | AAN algorithm, 16-bit |
| YCbCr->RGB565 | ~500 | 2.5% | Table lookup, no multiply |
| Bitstream refill | ~300 | 1.5% | Byte stuffing check |
| **Block total** | **~5,000** | **25.3%** | |
| Overhead (memcpy, loop) | ~500 | 2.5% | |
| **Total per block** | **~5,500** | **27.8%** | |

```
Total decode time: 5,500 * 2,250 = 12,375,000 cycles
At 667 MHz: 12,375,000 / 667,000,000 = 18.6 ms per frame

MARGIN: 44.5ms budget - 18.6ms decode = 25.9ms for USB + display
```

### Result: 400x240 @ 15 FPS is ACHIEVABLE

The numbers show ~19ms decode time with ~26ms margin for USB I/O and display blit.
At Q50, JPEG data size is ~12-15 KB per frame, well within USB bandwidth.

---

### What About 800x480 @ 15 FPS? (Full Resolution)

```
MCUs = (800/16) * (480/16) = 50 * 30 = 1,500 MCUs
Blocks = 1,500 * 6 = 9,000 blocks
Decode time: 5,500 * 9,000 = 49,500,000 cycles = 74.2 ms

Budget per frame: 66.7 ms (at 15 FPS)

RESULT: DOES NOT FIT. 74.2ms > 66.7ms budget.
```

Full resolution at 15 FPS is **marginally impossible**. Options:
- Drop to 10 FPS (100ms budget): fits with 26ms margin
- Use IFAST IDCT with aggressive shortcuts: might squeeze 12 FPS
- Decode at 400x240 and upscale 2x (recommended approach)

---

## H.264 Decode: Why It's DOA

For comparison, H.264 Baseline at 800x480@30fps:

| Component | Cycles | Notes |
|-----------|--------|-------|
| CAVLC entropy decode | ~50,000/MB | Much more complex than JPEG Huffman |
| Inverse transform (4x4) | ~3,000/MB | Similar to JPEG but more MBs |
| Motion compensation | ~40,000/MB | NO EQUIVALENT IN JPEG. This is the killer. |
| Deblocking filter | ~20,000/MB | Per-macroblock edge filtering |
| **Total per macroblock** | **~113,000** | |

```
800x480 at 16x16 MBs = 1,500 MBs
Per frame: 113,000 * 1,500 = 169,500,000 cycles = 254 ms

At 30 FPS budget: 33.3 ms
RESULT: 7.6x OVER BUDGET. Actual FPS: ~3.9

Even at 15 FPS (66.7ms budget): 3.8x over budget -> ~3.9 FPS
Even at 400x240 (375 MBs): 42,375,000 cycles = 63.5ms -> barely 15 FPS
  But add the overhead of H.264 framing and NAL parsing -> 10-12 FPS
```

**Conclusion**: H.264 software decode achieves 3-5 FPS at best. MJPEG wins.

---

## Memory Budget

### Total System Memory: 256 MB DDR2

```
Estimated OS + System Usage:
  WinCE 6.0 kernel + drivers:     ~40 MB
  MediaNav native processes:      ~40 MB  (GPS, radio, settings)
  MediaNav UI framework:          ~30 MB  (Qt/WinCE GUI)
  System buffers/caches:          ~20 MB
  -----------------------------------------------
  Available for our application:  ~126 MB (conservative)
```

### Our Application Memory Map

| Component | Bytes | KB | Notes |
|-----------|-------|----|-------|
| **Code (.text)** | ~200,000 | 195 | All compiled code |
| **Stack** | 65,536 | 64 | Main thread stack |
| **USB thread stack** | 65,536 | 64 | USB I/O thread |
| **USB RX buffer 0** | 65,536 | 64 | JPEG receive buffer |
| **USB RX buffer 1** | 65,536 | 64 | Double buffer |
| **USB TX buffer** | 4,096 | 4 | Touch event sends |
| **Ring buffer** | 131,072 | 128 | USB -> decode FIFO |
| **Decode buffer** | 192,000 | 188 | 400*240*2 RGB565 |
| **MJPEG context** | ~3,500 | 3.4 | Quant + Huffman tables |
| **Huffman lookups** | 2,560 | 2.5 | 4 tables * 256 * 2.5B avg |
| **Color LUTs** | 2,048 | 2 | Cb/Cr -> RGB deltas |
| **DCT block scratch** | 128 | 0.125 | 64 * int16_t |
| **Display back buffer** | 768,000 | 750 | 800*480*2 (if SW managed) |
| **Log buffer** | 1,024 | 1 | Debug log format buffer |
| | | | |
| **TOTAL** | **~1,566,000** | **~1,530** | **~1.5 MB** |

### Cache Utilization Analysis

```
16 KB I-Cache:
  Critical hot path (decode_block + IDCT + color_convert):
    decode_block:    ~800 bytes
    idct_ifast_mips: ~600 bytes
    huff_decode_dc:  ~200 bytes
    huff_decode_ac:  ~200 bytes
    color_convert:   ~400 bytes (4:2:0 path only)
    bits_refill:     ~100 bytes
    --------------------------------
    Total hot path:  ~2,300 bytes = 14.4% of I-cache
    
  VERDICT: Fits easily. No I-cache thrashing.

16 KB D-Cache:
  Per-block working set:
    DCT block (64 * 2B):          128 bytes
    Quant table (64 * 2B):        128 bytes
    Huffman DC lookup (256B):     256 bytes
    Huffman AC lookup (512B):     512 bytes
    Bitstream buffer (~32B):       32 bytes
    DC predictions (3 * 2B):        6 bytes
    Color LUTs (if converting):  2,048 bytes
    --------------------------------
    Total hot data:              ~3,110 bytes = 19.4% of D-cache
    
  VERDICT: Fits well. Color LUTs are the biggest consumer.
           Could split LUTs into hot/cold if needed.
```

---

## USB Bandwidth Analysis

```
USB 2.0 High Speed: 480 Mbit/s theoretical
Practical throughput: ~35 MB/s for bulk transfers

JPEG frame sizes (400x240):
  Q30:  ~8 KB/frame
  Q50:  ~15 KB/frame  (recommended)
  Q70:  ~30 KB/frame

At Q50, 15 FPS:
  15 * 15 KB = 225 KB/sec = 1.8 Mbit/s

USB utilization: 1.8 / 480 = 0.375%

VERDICT: USB bandwidth is NOT a bottleneck at all.
         We could stream 1080p MJPEG and still have USB bandwidth.
```

---

## Bottleneck Summary

```
                    BOTTLENECK MAP
    
    USB Bandwidth  [====                              ]  0.4%
    USB Latency    [==========                        ]  ~2ms per transfer
    JPEG Decode    [==========================        ]  ~19ms (18.6ms @ 400x240)
    Display Blit   [========                          ]  ~5ms (StretchDIBits)
    Touch Latency  [=====                             ]  ~3ms (message + USB send)
    Memory Usage   [==                                ]  1.5 MB / 126 MB
    
    CRITICAL PATH: JPEG Decode (IDCT + Huffman)
    SECONDARY:     Display Blit (GDI StretchDIBits scaling)
```

---

## Optimization Priorities

| Priority | Optimization | Expected Gain |
|----------|-------------|---------------|
| P0 | IFAST IDCT (already implemented) | 40% faster than ISLOW |
| P0 | All-zero block shortcut | 30-60% of blocks at Q50 |
| P1 | 8-bit Huffman lookup | Eliminates tree traversal for 90%+ codes |
| P1 | Color conversion LUTs | Eliminates 3 multiplies per pixel |
| P2 | Half-resolution decode + 2x upscale | 4x fewer blocks to decode |
| P2 | Fused dequantize + IDCT | Saves one pass over block data |
| P3 | MIPS-specific: unrolled IDCT | Saves loop overhead |
| P3 | Aligned memory access | Avoid unaligned load penalties |
| P4 | Skip chroma decode for static areas | Saves 2/6 of block decodes |
| P4 | Pre-process JPEG to remove byte stuffing | Removes branch from inner VLD loop |

---

## Realistic Performance Expectations

| Resolution | Quality | FPS | Decode Time | Usability |
|-----------|---------|-----|-------------|-----------|
| 200x120 | Q30 | 25-30 | ~5ms | Very blocky but responsive |
| 400x240 | Q50 | 15-20 | ~19ms | **Recommended sweet spot** |
| 400x240 | Q70 | 12-15 | ~25ms | Good quality, acceptable FPS |
| 800x480 | Q30 | 10-12 | ~55ms | Full res, low quality |
| 800x480 | Q50 | 8-10 | ~74ms | Full res, borderline usable |
| 800x480 | Q70 | 5-7 | ~100ms | Slideshow |
