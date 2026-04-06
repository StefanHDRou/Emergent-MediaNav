# MediaNav Android Auto Client (MN1-AA)

## A Custom, Lightweight Android Auto Client for Renault/Dacia MediaNav Standard (MN1)

**Target:** Windows CE 6.0 | MIPS (Alchemy Au1320 @ 667MHz) | 256MB RAM | No HW Video Decode

---

## BRUTAL HONESTY UPFRONT

Before you read another line, here is the cold, hard truth about this project:

### The H.264 Problem
Android Auto Protocol (AAP) **only supports H.264 Baseline, VP9, AV1, and H.265** as video codecs.
There is **NO MJPEG option** in the protocol. The `MediaCodecType` enum from the AAP protobuf spec is:

```
MEDIA_CODEC_VIDEO_H264_BP = 3;
MEDIA_CODEC_VIDEO_VP9     = 5;
MEDIA_CODEC_VIDEO_AV1     = 6;
MEDIA_CODEC_VIDEO_H265    = 7;
```

Software-decoding H.264 Baseline at 800x480@30fps on a 667MHz in-order MIPS core with
16KB I-cache and 16KB D-cache is **effectively impossible at usable framerates**. Expect
2-5 FPS at best. This is Dead On Arrival (DOA) for standard AAP video.

### The Solution: Dual-Path Architecture

This project implements **two paths**:

| Path | Protocol | Video Codec | Feasibility | FPS Target |
|------|----------|-------------|-------------|------------|
| **Path A: Standard AAP** | Full AAP over USB AOA | H.264 BP (software ffh264) | DOA: ~2-5 FPS | Slideshow |
| **Path B: Companion MJPEG** | Custom protocol over USB AOA | MJPEG (optimized libjpeg) | **VIABLE**: 12-20 FPS | Usable |

**Path B** is the recommended approach. It uses a custom Android companion app that:
1. Captures the Android Auto UI on the phone via MediaProjection API
2. Encodes frames as JPEG at configurable quality (Q40-Q60)
3. Streams over USB using a lightweight custom framing protocol
4. The MN1 decodes MJPEG and renders via DirectDraw/GDI

This sacrifices full AAP compliance for **actually working video** on this hardware.

---

## Documentation Index

| Document | Description |
|----------|-------------|
| [01 - Toolchain Setup](docs/01_TOOLCHAIN_SETUP.md) | Setting up VS2005 + Platform Builder for MIPS WinCE 6.0 |
| [02 - Architecture](docs/02_ARCHITECTURE.md) | Full software architecture, data flow, thread model |
| [03 - Performance Analysis](docs/03_PERFORMANCE_ANALYSIS.md) | Memory budgets, CPU cycle analysis, bottleneck map |
| [04 - Touch Input](docs/04_TOUCH_INPUT.md) | Touch screen routing back to phone via AOA/custom protocol |

## Project Structure

```
medianav-aa-client/
|-- README.md                          # This file
|-- docs/
|   |-- 01_TOOLCHAIN_SETUP.md         # Toolchain & SDK guide
|   |-- 02_ARCHITECTURE.md            # Software architecture
|   |-- 03_PERFORMANCE_ANALYSIS.md    # Performance & memory analysis
|   |-- 04_TOUCH_INPUT.md             # Touch input routing
|
|-- src/
|   |-- Makefile                       # Build system (VS2005 NMAKE)
|   |-- config.h                       # Build-time configuration
|   |-- types.h                        # Common type definitions
|   |
|   |-- usb/
|   |   |-- usb_host.h                 # WinCE USB Host abstraction
|   |   |-- usb_host.c                 # USB Host driver interface
|   |   |-- aoa_handshake.h            # AOA protocol handshake
|   |   |-- aoa_handshake.c            # AOA implementation
|   |
|   |-- protocol/
|   |   |-- aap_framing.h             # AAP frame header encode/decode
|   |   |-- aap_framing.c
|   |   |-- aap_service.h             # Service discovery & channel mgmt
|   |   |-- aap_service.c
|   |   |-- pb_lite.h                 # Minimal protobuf encoder/decoder
|   |   |-- pb_lite.c                 # (no external deps, hand-rolled)
|   |   |-- custom_protocol.h         # Custom MJPEG streaming protocol
|   |   |-- custom_protocol.c
|   |
|   |-- video/
|   |   |-- mjpeg_decoder.h           # MJPEG decoder interface
|   |   |-- mjpeg_decoder.c           # Hyper-optimized MIPS MJPEG decode
|   |   |-- jpeg_idct_mips.h          # MIPS-optimized IDCT
|   |   |-- jpeg_idct_mips.c          # 16-bit fixed-point IDCT
|   |   |-- jpeg_huffman.h            # Optimized Huffman decoder
|   |   |-- jpeg_huffman.c
|   |   |-- color_convert.h           # YCbCr -> RGB565 fast conversion
|   |   |-- color_convert.c
|   |
|   |-- display/
|   |   |-- display.h                 # Display output abstraction
|   |   |-- display_ddraw.c           # DirectDraw rendering path
|   |   |-- display_gdi.c             # GDI fallback rendering path
|   |
|   |-- input/
|   |   |-- touch_input.h             # Touch screen input handler
|   |   |-- touch_input.c             # WinCE touch -> protocol routing
|   |
|   |-- tls/
|   |   |-- tls_minimal.h             # Minimal TLS 1.2 wrapper
|   |   |-- tls_minimal.c             # For AAP encrypted channels
|   |
|   |-- util/
|   |   |-- ring_buffer.h             # Lock-free ring buffer
|   |   |-- ring_buffer.c
|   |   |-- debug_log.h               # Debug logging (serial/file)
|   |   |-- debug_log.c
|   |
|   |-- main.c                        # Application entry point
|
|-- companion-app/
|   |-- README.md                      # Android companion app concept
|
|-- tools/
|   |-- usb_test.c                     # USB connectivity test tool
```

## Quick Start (After Toolchain Setup)

```batch
REM Open VS2005 Command Prompt with MediaNav MIPS SDK
REM Navigate to src/ directory

nmake /f Makefile CFG=RELEASE

REM Copy mn1aa.exe to USB stick (FAT32)
REM On MediaNav: Enter Test Mode -> Run via Total Commander
```

## Hardware Reference

| Component | Specification |
|-----------|---------------|
| Processor | Alchemy Au1320, 667 MHz, MIPS32 |
| Pipeline | 5-stage in-order, single-issue |
| I-Cache | 16 KB, 4-way set-associative |
| D-Cache | 16 KB, 4-way write-back, read-allocate |
| RAM | 256 MB DDR2-667 (16-bit bus) |
| USB | USB 2.0 + OTG |
| Display | 800x480 TFT (assumed) |
| Touch | Resistive (assumed) |
| GPU/VPU | **NONE** - No hardware video acceleration confirmed |

## License

This project is provided for educational and research purposes. Android Auto is a trademark
of Google LLC. Use at your own risk - modifying your MediaNav may void your warranty.
