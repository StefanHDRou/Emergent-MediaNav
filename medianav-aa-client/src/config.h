/*
 * config.h - Build-time Configuration for MN1 Android Auto Client
 *
 * Target: Renault/Dacia MediaNav MN1
 *         Windows CE 6.0 / MIPS (Alchemy Au1320 @ 667MHz)
 *
 * CRITICAL: Every byte and every cycle counts on this platform.
 *           All values here are tuned for the Au1320's constraints:
 *           - 16KB I-cache / 16KB D-cache (4-way set-associative)
 *           - 256MB DDR2-667 (16-bit bus = ~1.3 GB/s peak bandwidth)
 *           - No SIMD, no FPU worth using, no HW video decode
 */

#ifndef MN1AA_CONFIG_H
#define MN1AA_CONFIG_H

/* =========================================================================
 * OPERATING MODE
 * ========================================================================= */

/*
 * MN1_MODE_AAP_STANDARD:
 *   Full Android Auto Protocol. Receives H.264 Baseline from phone.
 *   REALITY CHECK: Expect 2-5 FPS on this CPU. Unusable for driving.
 *   Enable only for protocol testing/development.
 *
 * MN1_MODE_COMPANION_MJPEG:
 *   Custom companion protocol. Android app captures screen, encodes
 *   as JPEG, streams over USB. MN1 decodes MJPEG.
 *   TARGET: 12-20 FPS at 800x480 Q50. This is the viable path.
 */
#define MN1_MODE_COMPANION_MJPEG    1
#define MN1_MODE_AAP_STANDARD       2

#ifndef MN1_OPERATING_MODE
#define MN1_OPERATING_MODE          MN1_MODE_COMPANION_MJPEG
#endif

/* =========================================================================
 * DISPLAY CONFIGURATION
 * ========================================================================= */

#define MN1_DISPLAY_WIDTH           800
#define MN1_DISPLAY_HEIGHT          480

/*
 * Pixel format: RGB565 (16-bit) is the only sane choice.
 * RGB888 would double memory bandwidth requirements.
 * Each frame = 800 * 480 * 2 = 768,000 bytes = 750 KB
 */
#define MN1_PIXEL_FORMAT_RGB565     1
#define MN1_BYTES_PER_PIXEL         2
#define MN1_FRAME_SIZE_BYTES        (MN1_DISPLAY_WIDTH * MN1_DISPLAY_HEIGHT * MN1_BYTES_PER_PIXEL)

/*
 * Display backend selection (compile-time):
 *   DDRAW  - DirectDraw (preferred: hardware blit if available)
 *   GDI    - GDI with StretchDIBits (fallback: always works)
 */
#define MN1_DISPLAY_DDRAW           1
#define MN1_DISPLAY_GDI             2

#ifndef MN1_DISPLAY_BACKEND
#define MN1_DISPLAY_BACKEND         MN1_DISPLAY_DDRAW
#endif

/* =========================================================================
 * VIDEO DECODE CONFIGURATION (MJPEG PATH)
 * ========================================================================= */

/*
 * JPEG quality the companion app should encode at.
 * Lower = faster decode, less bandwidth, worse quality.
 *
 * Q30: ~15 KB/frame, very blocky but fast
 * Q50: ~25 KB/frame, acceptable quality (RECOMMENDED)
 * Q70: ~45 KB/frame, good quality but pushes bandwidth
 *
 * At 800x480 Q50 @ 15 FPS: ~375 KB/s = ~3 Mbit/s
 * USB 2.0 High Speed: 480 Mbit/s theoretical, ~35 MB/s real
 * Bandwidth is NOT the bottleneck. CPU decode time IS.
 */
#define MN1_JPEG_DEFAULT_QUALITY    50

/*
 * Decode resolution. Can be lower than display for speed,
 * then upscaled via nearest-neighbor blit.
 *
 * FULL:    800x480 decode (1:1 with display)
 * HALF:    400x240 decode, 2x upscale (4x fewer pixels to decode)
 * QUARTER: 200x120 decode, 4x upscale (16x fewer pixels, very blocky)
 */
#define MN1_DECODE_FULL             1
#define MN1_DECODE_HALF             2
#define MN1_DECODE_QUARTER          3

#ifndef MN1_DECODE_RESOLUTION
#define MN1_DECODE_RESOLUTION       MN1_DECODE_HALF
#endif

#if MN1_DECODE_RESOLUTION == MN1_DECODE_FULL
  #define MN1_DECODE_WIDTH          800
  #define MN1_DECODE_HEIGHT         480
  #define MN1_DECODE_SCALE          1
#elif MN1_DECODE_RESOLUTION == MN1_DECODE_HALF
  #define MN1_DECODE_WIDTH          400
  #define MN1_DECODE_HEIGHT         240
  #define MN1_DECODE_SCALE          2
#elif MN1_DECODE_RESOLUTION == MN1_DECODE_QUARTER
  #define MN1_DECODE_WIDTH          200
  #define MN1_DECODE_HEIGHT         120
  #define MN1_DECODE_SCALE          4
#endif

/*
 * IDCT precision:
 *   ISLOW: 16-bit integer slow IDCT (accurate, ~40% of decode time)
 *   IFAST: 16-bit integer fast IDCT (less accurate, ~25% of decode time)
 *
 * IFAST introduces visible artifacts but saves significant CPU.
 * For a car dashboard at arm's length, IFAST is acceptable.
 */
#define MN1_IDCT_ISLOW              1
#define MN1_IDCT_IFAST              2

#ifndef MN1_IDCT_METHOD
#define MN1_IDCT_METHOD             MN1_IDCT_IFAST
#endif

/* =========================================================================
 * USB CONFIGURATION
 * ========================================================================= */

/*
 * USB buffer sizes. Tuned for the Au1320's USB 2.0 controller.
 *
 * The USB controller on Au1320 supports USB 2.0 High Speed (480 Mbit/s).
 * Bulk endpoint max packet size: 512 bytes (HS).
 *
 * RX buffer: Should be multiple of 512 and large enough to hold
 * at least one JPEG frame to avoid USB stalls during decode.
 *
 * Double-buffering: While one buffer is being decoded, the other
 * receives the next frame from USB.
 */
#define MN1_USB_RX_BUFFER_SIZE      (64 * 1024)   /* 64 KB per buffer */
#define MN1_USB_TX_BUFFER_SIZE      (4  * 1024)    /* 4 KB (touch events are small) */
#define MN1_USB_NUM_RX_BUFFERS      2              /* Double-buffer */

/* USB Vendor/Product IDs for AOA */
#define MN1_USB_GOOGLE_VID          0x18D1
#define MN1_USB_AOA_PID             0x2D00
#define MN1_USB_AOA_ADB_PID        0x2D01

/* AOA control request codes */
#define MN1_AOA_GET_PROTOCOL        51
#define MN1_AOA_SEND_STRING         52
#define MN1_AOA_START               53

/* AOA string indices */
#define MN1_AOA_STR_MANUFACTURER    0
#define MN1_AOA_STR_MODEL           1
#define MN1_AOA_STR_DESCRIPTION     2
#define MN1_AOA_STR_VERSION         3
#define MN1_AOA_STR_URI             4
#define MN1_AOA_STR_SERIAL          5

/* =========================================================================
 * AAP PROTOCOL CONFIGURATION (Path A - Standard AAP)
 * ========================================================================= */

/*
 * AAP frame header format:
 *   [Channel 1B] [Flags 1B] [FrameLen 2B BE] [TotalLen 4B BE if first]
 *
 * Max frame payload: 0x4000 (16384 bytes)
 */
#define MN1_AAP_MAX_FRAME_SIZE      0x4000
#define MN1_AAP_HEADER_SIZE         4       /* Without total length field */
#define MN1_AAP_HEADER_SIZE_FIRST   8       /* With total length field */

/* AAP protocol version */
#define MN1_AAP_VERSION_MAJOR       1
#define MN1_AAP_VERSION_MINOR       6

/* Video configuration we advertise to the phone */
#define MN1_AAP_VIDEO_CODEC         3       /* MEDIA_CODEC_VIDEO_H264_BP */
#define MN1_AAP_VIDEO_RESOLUTION    1       /* VIDEO_800x480 */
#define MN1_AAP_VIDEO_FPS           2       /* VIDEO_FPS_30 */

/* =========================================================================
 * CUSTOM COMPANION PROTOCOL (Path B - MJPEG)
 * ========================================================================= */

/*
 * Custom frame header (8 bytes, minimal overhead):
 *
 * Offset  Size  Field
 * 0       2     Magic: 0xAA55
 * 2       1     Frame type (0x01=video, 0x02=touch_ack, 0x03=config)
 * 3       1     Flags (bit0=keyframe, always 1 for MJPEG)
 * 4       4     Payload length (little-endian, max 256KB)
 *
 * Total overhead per frame: 8 bytes. At 15 FPS = 120 bytes/sec.
 */
#define MN1_CUSTOM_MAGIC            0xAA55
#define MN1_CUSTOM_FRAME_VIDEO      0x01
#define MN1_CUSTOM_FRAME_TOUCH      0x02
#define MN1_CUSTOM_FRAME_CONFIG     0x03
#define MN1_CUSTOM_FRAME_TOUCH_ACK  0x04
#define MN1_CUSTOM_HEADER_SIZE      8

/* =========================================================================
 * MEMORY BUDGET
 * ========================================================================= */

/*
 * Total available: ~256 MB
 * WinCE kernel + drivers: ~40-60 MB (estimated)
 * MediaNav native processes: ~30-50 MB (estimated)
 * Available for our app: ~150-180 MB (conservative: 150 MB)
 *
 * Our budget (targeting < 8 MB total):
 *
 * Component              Bytes        Notes
 * ---------------------------------------------------------
 * USB RX buffers (x2)    131,072      64KB * 2 double-buffer
 * USB TX buffer           4,096       Touch events
 * Decode workspace       307,200      400*240*2 (half-res RGB565) + scratch
 * Display framebuffer    768,000      800*480*2 RGB565 (if we manage our own)
 * JPEG decode tables      4,096       Huffman + quantization tables
 * IDCT row buffer            512      One 8-pixel row workspace
 * Ring buffer (USB->decode) 131,072   64KB ring for frame assembly
 * TLS buffers (AAP only)  32,768      Only if using standard AAP
 * Stack                   65,536      64KB stack
 * Code (.text)           ~200,000     Estimated ~200KB for all code
 * ---------------------------------------------------------
 * TOTAL                  ~1.6 MB      Well within budget
 *
 * NOTE: If DirectDraw manages the framebuffer in VRAM, we save 750KB.
 */

#define MN1_STACK_SIZE              (64 * 1024)

/* =========================================================================
 * THREADING MODEL
 * ========================================================================= */

/*
 * Two threads only (minimize context switch overhead):
 *
 * Thread 1 (USB I/O): Handles USB bulk reads/writes.
 *   - Reads JPEG frames into ring buffer
 *   - Sends touch events to phone
 *   - Priority: THREAD_PRIORITY_ABOVE_NORMAL
 *
 * Thread 2 (Decode + Render): Main loop.
 *   - Reads complete frames from ring buffer
 *   - Decodes JPEG -> RGB565
 *   - Blits to display
 *   - Reads touch input, queues for Thread 1
 *   - Priority: THREAD_PRIORITY_HIGHEST
 */
#define MN1_THREAD_PRIORITY_USB     THREAD_PRIORITY_ABOVE_NORMAL
#define MN1_THREAD_PRIORITY_RENDER  THREAD_PRIORITY_HIGHEST

/* =========================================================================
 * DEBUG / LOGGING
 * ========================================================================= */

/*
 * Log levels:
 *   0 = OFF (release builds, saves ~5KB code + branch prediction slots)
 *   1 = ERROR only
 *   2 = ERROR + WARN
 *   3 = ERROR + WARN + INFO (debug builds)
 */
#ifndef MN1_LOG_LEVEL
#define MN1_LOG_LEVEL               0
#endif

/* Log to file on Storage Card (slow!) or serial port (fast) */
#define MN1_LOG_TARGET_NONE         0
#define MN1_LOG_TARGET_FILE         1
#define MN1_LOG_TARGET_SERIAL       2

#ifndef MN1_LOG_TARGET
#define MN1_LOG_TARGET              MN1_LOG_TARGET_NONE
#endif

#define MN1_LOG_FILE_PATH           L"\\Storage Card\\mn1aa.log"

/* =========================================================================
 * AOA ACCESSORY IDENTITY STRINGS
 * ========================================================================= */

#define MN1_AOA_MANUFACTURER        "MediaNav"
#define MN1_AOA_MODEL               "MN1-AA-Client"
#define MN1_AOA_DESCRIPTION         "MediaNav Android Auto Client"
#define MN1_AOA_VERSION             "1.0"
#define MN1_AOA_URI                 ""
#define MN1_AOA_SERIAL              "MN1-001"

#endif /* MN1AA_CONFIG_H */
