/*
 * types.h - Common Type Definitions for MN1 Android Auto Client
 *
 * Provides platform-independent types and WinCE-specific wrappers.
 * This header is included by every source file.
 */

#ifndef MN1AA_TYPES_H
#define MN1AA_TYPES_H

#include <windows.h>

/* =========================================================================
 * FIXED-WIDTH INTEGER TYPES
 * ========================================================================= */

/* WinCE 6.0 / VS2005 does not have <stdint.h>. Define manually. */
typedef unsigned char       uint8_t;
typedef signed char         int8_t;
typedef unsigned short      uint16_t;
typedef signed short        int16_t;
typedef unsigned int        uint32_t;
typedef signed int          int32_t;
typedef unsigned __int64    uint64_t;
typedef signed __int64      int64_t;

typedef uint32_t            size_t_mn1;  /* Avoid conflict with WinCE size_t */

/* Boolean (WinCE BOOL is int, but we want minimal) */
#ifndef TRUE
#define TRUE    1
#endif
#ifndef FALSE
#define FALSE   0
#endif

typedef int                 mn1_bool_t;

/* =========================================================================
 * RESULT CODES
 * ========================================================================= */

typedef enum {
    MN1_OK                      = 0,
    MN1_ERR_GENERIC             = -1,
    MN1_ERR_USB_INIT            = -10,
    MN1_ERR_USB_NO_DEVICE       = -11,
    MN1_ERR_USB_TRANSFER        = -12,
    MN1_ERR_USB_AOA_UNSUPPORTED = -13,
    MN1_ERR_USB_AOA_HANDSHAKE   = -14,
    MN1_ERR_AAP_VERSION         = -20,
    MN1_ERR_AAP_AUTH            = -21,
    MN1_ERR_AAP_SERVICE         = -22,
    MN1_ERR_AAP_CHANNEL         = -23,
    MN1_ERR_DISPLAY_INIT        = -30,
    MN1_ERR_DISPLAY_BLIT        = -31,
    MN1_ERR_DECODE_INVALID      = -40,
    MN1_ERR_DECODE_TRUNCATED    = -41,
    MN1_ERR_DECODE_UNSUPPORTED  = -42,
    MN1_ERR_OUT_OF_MEMORY       = -50,
    MN1_ERR_TLS_HANDSHAKE       = -60,
    MN1_ERR_TLS_RECORD          = -61,
    MN1_ERR_TIMEOUT             = -70,
} mn1_result_t;

/* =========================================================================
 * COMMON STRUCTURES
 * ========================================================================= */

/* USB endpoint pair discovered after AOA handshake */
typedef struct {
    HANDLE      hUSBDevice;         /* WinCE USB device handle */
    HANDLE      hBulkIn;            /* Bulk IN pipe (phone -> MN1) */
    HANDLE      hBulkOut;           /* Bulk OUT pipe (MN1 -> phone) */
    uint16_t    wMaxPacketIn;       /* Max packet size for IN endpoint */
    uint16_t    wMaxPacketOut;      /* Max packet size for OUT endpoint */
    mn1_bool_t  bConnected;         /* Connection state */
} mn1_usb_conn_t;

/* Decoded frame ready for display */
typedef struct {
    uint16_t*   pPixels;            /* RGB565 pixel data */
    uint16_t    wWidth;             /* Frame width */
    uint16_t    wHeight;            /* Frame height */
    uint32_t    dwTimestamp;         /* Capture timestamp (ms) */
    uint32_t    dwDecodeTimeUs;     /* Decode time (microseconds) */
} mn1_frame_t;

/* Touch event to send back to phone */
typedef struct {
    uint16_t    x;                  /* Touch X (0 - display width) */
    uint16_t    y;                  /* Touch Y (0 - display height) */
    uint8_t     action;             /* 0=DOWN, 1=UP, 2=MOVE */
    uint8_t     pointerId;          /* Multi-touch pointer ID */
    uint64_t    timestamp;          /* Timestamp in nanoseconds */
} mn1_touch_event_t;

/* Custom protocol frame header (8 bytes) */
#pragma pack(push, 1)
typedef struct {
    uint16_t    wMagic;             /* 0xAA55 */
    uint8_t     bFrameType;         /* Frame type (video/touch/config) */
    uint8_t     bFlags;             /* Bit flags */
    uint32_t    dwPayloadLen;       /* Payload length (little-endian) */
} mn1_custom_header_t;
#pragma pack(pop)

/* AAP frame header (4 or 8 bytes) */
#pragma pack(push, 1)
typedef struct {
    uint8_t     bChannel;           /* Channel ID */
    uint8_t     bFlags;             /* Fragment flags */
    uint16_t    wFrameLen;          /* Frame payload length (big-endian) */
    /* If flags indicate first fragment: */
    /* uint32_t dwTotalLen;          Total packet length (big-endian) */
} mn1_aap_header_t;
#pragma pack(pop)

/* AAP fragment flags */
#define MN1_AAP_FLAG_FIRST      0x01
#define MN1_AAP_FLAG_LAST       0x02
#define MN1_AAP_FLAG_UNFRAG     0x03  /* First + Last = single frame */

/* Ring buffer for inter-thread communication */
typedef struct {
    uint8_t*    pBuffer;            /* Backing memory */
    uint32_t    dwSize;             /* Total buffer size (must be power of 2) */
    volatile uint32_t dwHead;       /* Write position (USB thread) */
    volatile uint32_t dwTail;       /* Read position (decode thread) */
} mn1_ring_buffer_t;

/* Application global state */
typedef struct {
    mn1_usb_conn_t      usb;
    mn1_ring_buffer_t   rxRing;
    mn1_frame_t         frame;
    HANDLE              hUsbThread;
    HANDLE              hRenderThread;
    HANDLE              hFrameReady;    /* Event: new frame in ring buffer */
    HANDLE              hUsbReady;      /* Event: USB buffer space available */
    volatile mn1_bool_t bRunning;       /* App running flag */
    volatile uint32_t   dwFrameCount;   /* Total frames decoded */
    volatile uint32_t   dwDropCount;    /* Frames dropped */
    uint32_t            dwFps;          /* Current FPS (updated per second) */
} mn1_app_state_t;

/* =========================================================================
 * INLINE UTILITIES
 * ========================================================================= */

/* Big-endian read/write (AAP uses big-endian) */
static __inline uint16_t mn1_read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static __inline uint32_t mn1_read_be32(const uint8_t* p) {
    return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static __inline void mn1_write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static __inline void mn1_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* Little-endian read (custom protocol uses LE) */
static __inline uint32_t mn1_read_le32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static __inline void mn1_write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Timing helper (WinCE GetTickCount is ms resolution) */
static __inline uint32_t mn1_get_tick_ms(void) {
    return GetTickCount();
}

#endif /* MN1AA_TYPES_H */
