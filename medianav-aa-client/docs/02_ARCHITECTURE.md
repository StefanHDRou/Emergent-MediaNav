# 02 - Software Architecture

## System Architecture Overview

```
+-------------------+          USB 2.0 HS           +-------------------+
|   ANDROID PHONE   | <===========================> |  MEDIANAV MN1     |
|                   |    AOA Bulk IN/OUT (480Mbps)   |  WinCE 6.0/MIPS  |
|  Companion App    |                                |  mn1aa.exe        |
|  - MediaProjection|   Phone->MN1: JPEG frames     |                   |
|  - JPEG Encoder   |   MN1->Phone: Touch events    |  - MJPEG Decoder  |
|  - Touch Injector |   MN1->Phone: Config messages  |  - Display Output |
|  - AOA USB Device |                                |  - Touch Capture  |
+-------------------+                                +-------------------+
```

---

## Data Flow

```
Phone Side                          MN1 Side
==========                          ========

MediaProjection API                 USB Bulk IN
    |                                   |
    v                                   v
Screen Capture (bitmap)             USB I/O Thread
    |                                   |
    v                                   v
JPEG Encoder (Q50)                  Ring Buffer (128KB)
    |                                   |
    v                                   v
Custom Frame Header (8B)            Main Thread: Read frame
    |                                   |
    v                                   v
USB Bulk OUT                        JPEG Decode Pipeline:
                                      1. Parse headers (DQT, DHT, SOF, SOS)
                                      2. Huffman decode (VLD)
                                      3. Dequantize
                                      4. IFAST IDCT (8x8 blocks)
                                      5. YCbCr -> RGB565 (fused)
                                         |
                                         v
                                    Display Blit (DirectDraw/GDI)
                                      - StretchBlt if decode != display res
                                         |
                                         v
                                    800x480 TFT Screen
```

---

## Threading Model

```
+-----------------------------------------+
|            MAIN THREAD                   |
|  Priority: THREAD_PRIORITY_HIGHEST      |
|                                         |
|  1. PeekMessage (touch/WM_QUIT)        |
|  2. WaitForSingleObject(hFrameReady)    |
|  3. Ring buffer read (JPEG frame)       |
|  4. mjpeg_decode_frame()                |
|  5. mn1_display_blit()                  |
|  6. FPS counter update                  |
|  7. goto 1                              |
+-----------------------------------------+

+-----------------------------------------+
|            USB THREAD                    |
|  Priority: THREAD_PRIORITY_ABOVE_NORMAL |
|                                         |
|  1. Check for pending touch event       |
|  2. mn1_custom_send_touch() if pending  |
|  3. mn1_custom_read_video_frame()       |
|  4. Ring buffer write (JPEG data)       |
|  5. SetEvent(hFrameReady)               |
|  6. goto 1                              |
+-----------------------------------------+
```

**Why only 2 threads?**
- Au1320 is single-core: more threads = more context switches = wasted cycles
- USB I/O blocks on bulk reads anyway (natural yielding)
- Main thread can process messages during WaitForSingleObject timeout
- Zero shared state except: ring buffer (lock-free SPSC) + touch event (atomic-ish)

---

## Module Dependency Graph

```
main.c
  |
  +-- display/display.h       (display init, blit, lock/unlock)
  |     |-- display_ddraw.c   (DirectDraw + GDI fallback)
  |
  +-- input/touch_input.h     (touch capture, WM_LBUTTON*)
  |     |-- touch_input.c
  |
  +-- usb/usb_host.h          (USB Host, OTG switch, bulk I/O)
  |     |-- usb_host.c
  |     |-- aoa_handshake.h   (AOA protocol)
  |           |-- aoa_handshake.c
  |
  +-- protocol/
  |     |-- custom_protocol.h (MJPEG streaming protocol)
  |     |     |-- custom_protocol.c
  |     |-- aap_framing.h     (AAP frame layer - standard path)
  |     |     |-- aap_framing.c
  |     |-- aap_service.h     (AAP services - standard path stub)
  |     |-- pb_lite.h         (minimal protobuf)
  |           |-- pb_lite.c
  |
  +-- video/
  |     |-- mjpeg_decoder.h   (JPEG decode orchestration)
  |     |     |-- mjpeg_decoder.c
  |     |-- jpeg_idct_mips.h  (MIPS-optimized IDCT)
  |     |     |-- jpeg_idct_mips.c
  |     |-- jpeg_huffman.h    (fast Huffman decode)
  |     |     |-- jpeg_huffman.c
  |     |-- color_convert.h   (YCbCr -> RGB565)
  |           |-- color_convert.c
  |
  +-- util/
        |-- ring_buffer.h     (lock-free SPSC ring buffer)
        |     |-- ring_buffer.c
        |-- debug_log.h       (logging macros)
              |-- debug_log.c
```

---

## AOA Handshake Sequence Diagram

```
MN1 (USB Host)                              Android Phone (USB Device)
=============                               =========================

1. USB device detected
   (any VID:PID)
        |
        |---> GET_PROTOCOL (req 51) ------->|
        |<--- Protocol Version (1 or 2) <---|
        |
2. Send identity strings:
        |---> SEND_STRING[0] "MediaNav" --->|
        |---> SEND_STRING[1] "MN1-AA" ---->|
        |---> SEND_STRING[2] "MN1 AA" ---->|
        |---> SEND_STRING[3] "1.0" -------->|  (MUST NOT be empty!)
        |---> SEND_STRING[4] "" ----------->|
        |---> SEND_STRING[5] "MN1-001" --->|
        |
3. Start accessory mode:
        |---> START (req 53) -------------->|
        |                                    |
        |    *** Phone disconnects ***       |
        |    *** Phone reconnects as ***     |
        |    *** VID:18D1 PID:2D00   ***     |
        |                                    |
4. Re-enumeration:
        |<--- New device (18D1:2D00) -------|
        |
5. Claim interface 0, find bulk endpoints
        |==== Bulk IN/OUT established ======|
        |
6. Begin data exchange
        |<--- JPEG frames (Bulk IN) --------|
        |---> Touch events (Bulk OUT) ----->|
```

---

## Custom Protocol Wire Format

```
Frame structure (variable length):

+--------+--------+--------+--------+--------+--- ... ---+
| Magic  | Magic  | Type   | Flags  | Length (LE 32-bit) |  Payload
| 0x55   | 0xAA   | 1 byte | 1 byte | 4 bytes           |  N bytes
+--------+--------+--------+--------+--------+--- ... ---+
   0        1        2        3       4  5  6  7     8..N+7

Frame Types:
  0x01 = VIDEO   (Phone -> MN1: JPEG data, SOI to EOI)
  0x02 = TOUCH   (MN1 -> Phone: touch event struct)
  0x03 = CONFIG  (MN1 -> Phone: stream configuration)
  0x04 = TOUCH_ACK (Phone -> MN1: touch received)

Touch Event Payload (12 bytes):
  +0  uint16_t  x           (touch X coordinate)
  +2  uint16_t  y           (touch Y coordinate)
  +4  uint8_t   action      (0=DOWN, 1=UP, 2=MOVE)
  +5  uint8_t   pointer_id  (always 0 for single-touch)
  +6  uint64_t  timestamp   (nanoseconds)

Config Payload (8 bytes):
  +0  uint16_t  width       (requested capture width)
  +2  uint16_t  height      (requested capture height)
  +4  uint8_t   quality     (JPEG quality 1-100)
  +5  uint8_t   max_fps     (maximum frames per second)
  +6  uint8_t[2] reserved
```

---

## AAP Frame Format (Standard Path)

```
+----------+-------+-----------+-----------+--- ... ---+
| Channel  | Flags | Frame Len | Total Len | Payload   |
| 1 byte   | 1 byte| 2B (BE)   | 4B (BE)*  | N bytes   |
+----------+-------+-----------+-----------+--- ... ---+

* Total Len only present if Flags & FIRST

Flags:
  0x01 = FIRST (first fragment of multi-frame packet)
  0x02 = LAST  (last fragment)
  0x03 = UNFRAGMENTED (single frame)
  0x00 = CONTINUATION (middle fragment)

Max frame payload: 16384 bytes (0x4000)

Channel 0: Control (service discovery, auth, ping)
Channel N: Dynamically assigned service channels
```
