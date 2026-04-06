# MN1 Android Auto Client - Project Requirements Document

## Original Problem Statement
Create a custom, lightweight client for Android Auto that runs natively as an .exe on a Renault/Dacia MediaNav Standard (MN1) unit from 2014. Target: WinCE 6.0, MIPS Au1320 @ 667MHz, 256MB RAM, no HW video accel. Includes: toolchain setup guide, AOA protocol handshake, optimized MJPEG decode loop, touch input routing.

## User Choices
- **Deliverable**: Raw Markdown documentation files + full compilable project structure
- **Code Depth**: Full compilable project with Makefiles, headers, source files
- **Research**: Extensive web research for toolchains and libraries
- **Performance**: Include memory budget calculations, focus on MJPEG (H.264 DOA)
- **Scope**: Video + Touch only (no audio)

## Architecture
- **Path A (Standard AAP)**: H.264 over full AAP protocol - DOA at 2-5 FPS
- **Path B (Companion MJPEG)**: Custom protocol with MJPEG streaming - VIABLE at 15-20 FPS
- **Threading**: 2 threads (USB I/O + Decode/Render)
- **Memory**: ~1.5 MB total footprint
- **Display**: DirectDraw primary, GDI fallback

## What's Been Implemented (Jan 2026)

### Documentation (6 files)
- [x] README.md - Master project overview with brutal honesty about H.264 limitations
- [x] 01_TOOLCHAIN_SETUP.md - VS2005 + Platform Builder CE 6.0 MIPS SDK guide
- [x] 02_ARCHITECTURE.md - Full software architecture, data flow, thread model, wire formats
- [x] 03_PERFORMANCE_ANALYSIS.md - Cycle budget, memory budget, bottleneck analysis
- [x] 04_TOUCH_INPUT.md - Touch routing from WinCE to phone via custom protocol

### Source Code (35 files, ~6966 lines)
- [x] Makefile (NMAKE for VS2005 MIPS cross-compilation)
- [x] config.h (all build-time configuration with detailed comments)
- [x] types.h (fixed-width types, common structures, inline utilities)
- [x] USB/AOA layer (usb_host.c/h, aoa_handshake.c/h) - OTG switching, AOA handshake
- [x] Protocol layer (aap_framing, aap_service, custom_protocol, pb_lite)
- [x] Video decoder (mjpeg_decoder, jpeg_idct_mips, jpeg_huffman, color_convert)
- [x] Display output (DirectDraw + GDI backends)
- [x] Touch input (WinCE WM_LBUTTON subclass, coordinate scaling)
- [x] Utilities (lock-free ring buffer, debug logging)
- [x] main.c (complete application entry point with 12-step init sequence)
- [x] USB test tool (standalone diagnostics for MediaNav)

### Web Documentation Browser
- [x] FastAPI backend serving project files via REST API
- [x] React frontend with IDE-themed file tree + code/markdown viewer
- [x] JetBrains Mono font, dark Ayu-like color scheme

## Testing Status
- Backend: 100% (7/7 tests passed)
- Frontend: 100% (all UI tests passed)

## Prioritized Backlog

### P0 (Critical for working prototype)
- [ ] Android companion app (MediaProjection + JPEG encode + AOA device)
- [ ] Test on actual MediaNav hardware (verify USB OTG register addresses)
- [ ] Profile JPEG decode performance on real Au1320

### P1 (Important)
- [ ] JPEG restart marker support (for error resilience)
- [ ] Adaptive quality: auto-reduce JPEG quality if decode falls behind
- [ ] Frame dropping strategy: skip old frames when ring buffer is full
- [ ] Better Huffman slow-path (min-code table for codes > 8 bits)

### P2 (Nice to have)
- [ ] Audio pass-through (would need separate USB endpoint or BT A2DP)
- [ ] BearSSL port for TLS (enables standard AAP path)
- [ ] MIPS assembly IDCT (hand-tuned for Au1320 pipeline)
- [ ] Direct framebuffer DMA if Au1320 supports it

## Key Risks
1. Au1320 USB OTG register addresses may differ from documented Au1xxx family
2. MediaNav BSP may block VirtualCopy to hardware registers
3. Resistive touch calibration may be off in our fullscreen window
4. Companion app touch injection may require root on newer Android versions
