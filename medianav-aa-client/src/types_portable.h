/*
 * types_portable.h - Portable Type Definitions (Linux/POSIX)
 *
 * Replaces types.h for non-WinCE builds. Provides the same types
 * and structures using standard C library instead of Windows APIs.
 */

#ifndef MN1AA_TYPES_PORTABLE_H
#define MN1AA_TYPES_PORTABLE_H

#ifdef PORTABLE_BUILD

/* POSIX feature test macros - must be before any includes */
#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* WinCE types that don't exist on POSIX */
typedef int BOOL;
typedef void* HANDLE;
typedef void* HWND;
typedef void* HINSTANCE;
typedef unsigned long DWORD;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

typedef int mn1_bool_t;

/* Result codes */
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

/* USB connection stub (not used in portable builds) */
typedef struct {
    HANDLE      hUSBDevice;
    HANDLE      hBulkIn;
    HANDLE      hBulkOut;
    uint16_t    wMaxPacketIn;
    uint16_t    wMaxPacketOut;
    mn1_bool_t  bConnected;
} mn1_usb_conn_t;

/* Touch event */
typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint8_t     action;
    uint8_t     pointerId;
    uint64_t    timestamp;
} mn1_touch_event_t;

/* Custom protocol header */
#pragma pack(push, 1)
typedef struct {
    uint16_t    wMagic;
    uint8_t     bFrameType;
    uint8_t     bFlags;
    uint32_t    dwPayloadLen;
} mn1_custom_header_t;
#pragma pack(pop)

/* Ring buffer */
typedef struct {
    uint8_t*    pBuffer;
    uint32_t    dwSize;
    volatile uint32_t dwHead;
    volatile uint32_t dwTail;
} mn1_ring_buffer_t;

/* Inline utilities */
static inline uint16_t mn1_read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t mn1_read_be32(const uint8_t* p) {
    return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static inline void mn1_write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v);
}

static inline void mn1_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}

static inline uint32_t mn1_read_le32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static inline void mn1_write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* WinCE API stubs */
static inline uint32_t GetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#define mn1_get_tick_ms GetTickCount

/* Memory allocation using stdlib (replaces WinCE LocalAlloc) */
#define LocalAlloc(flags, size)  malloc(size)
#define LocalFree(ptr)           free(ptr)
#define LMEM_FIXED               0

/* Config defines needed by modules */
#ifndef MN1_CUSTOM_MAGIC
#define MN1_CUSTOM_MAGIC         0xAA55
#define MN1_CUSTOM_FRAME_VIDEO   0x01
#define MN1_CUSTOM_FRAME_TOUCH   0x02
#define MN1_CUSTOM_FRAME_CONFIG  0x03
#define MN1_CUSTOM_HEADER_SIZE   8
#endif

#endif /* PORTABLE_BUILD */
#endif /* MN1AA_TYPES_PORTABLE_H */
