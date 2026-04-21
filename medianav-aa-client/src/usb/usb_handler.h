/*
 * usb_handler.h - User-Mode USB Host Stack for MediaNav MN1
 *
 * PROBLEM CONTEXT:
 *   The MediaNav WinCE 6.0 image is stripped. There is:
 *   - NO Plug-and-Play (PnP) manager
 *   - NO usbser.dll (serial class driver)
 *   - A background process (MgrUSB.exe) that exclusively claims the USB port
 *     for Mass Storage devices only
 *   - The USBD.dll may be present but useless without PnP to dispatch
 *     device-attach notifications to registered client drivers
 *
 * SOLUTION:
 *   We implement a User-Mode USB Host Stack that:
 *   1. Kills MgrUSB.exe to release the USB port
 *   2. Attempts to open the HCD stream driver (HCD1:) directly
 *   3. Falls back to direct EHCI register access via VirtualCopy if HCD
 *      is not exposable as a stream device
 *   4. Performs manual USB device enumeration (GET_DESCRIPTOR, SET_ADDRESS)
 *   5. Executes the full AOA v2.0 handshake using raw control transfers
 *   6. Opens bulk IN/OUT pipes and spawns high-priority worker threads
 *
 * ARCHITECTURE:
 *
 *   +---------------------------------------------------------+
 *   |               usb_handler.cpp (this module)             |
 *   |                                                         |
 *   |  +-------------------+  +----------------------------+  |
 *   |  | HCD Access Layer  |  | EHCI Register Layer        |  |
 *   |  | (CreateFile HCD1:)|  | (VirtualCopy MMIO fallback)|  |
 *   |  +-------------------+  +----------------------------+  |
 *   |            |                       |                    |
 *   |  +---------v-----------------------v-----------+        |
 *   |  |          USB Core / Transfer Engine          |        |
 *   |  |  - Control transfers (Setup + Data + Status) |        |
 *   |  |  - Bulk transfers (IN / OUT, async capable)  |        |
 *   |  +---------------------------------------------+        |
 *   |            |                                             |
 *   |  +---------v----------------------------+                |
 *   |  |     AOA v2.0 Protocol Engine         |                |
 *   |  |  - GET_PROTOCOL / SEND_STRING / START|                |
 *   |  |  - Re-enumeration wait               |                |
 *   |  |  - Endpoint discovery                |                |
 *   |  +-------------------------------------+                |
 *   |            |                                             |
 *   |  +---------v----------------------------+                |
 *   |  |     Bulk Endpoint Worker Threads     |                |
 *   |  |  - Video IN thread (high priority)   |                |
 *   |  |  - Control/Touch OUT thread          |                |
 *   |  +-------------------------------------+                |
 *   +---------------------------------------------------------+
 *
 * TARGET: MIPS (Alchemy Au1320), WinCE 6.0
 * REQUIRES: No external dependencies beyond WinCE core APIs
 */

#ifndef MN1AA_USB_HANDLER_H
#define MN1AA_USB_HANDLER_H

#include "config.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * EHCI REGISTER DEFINITIONS (Au1320-specific)
 *
 * The Au1320 uses an EHCI-compatible USB 2.0 host controller.
 * Physical base addresses discovered via BSP analysis:
 *   - USB Host EHCI regs: 0x14020100 (operational registers)
 *   - USB Host capability regs: 0x14020000
 *
 * These MAY differ on production MediaNav units. The code probes
 * multiple known addresses.
 * ========================================================================= */

/* Known Au1320/Au1xxx USB physical base addresses to probe */
#define AU1320_USB_PHYS_BASE_0      0x14020000
#define AU1320_USB_PHYS_BASE_1      0x14021000  /* Alternate mapping */
#define AU1320_USB_PHYS_BASE_2      0x10100000  /* Some Au1100 variants */
#define AU1320_EHCI_OP_OFFSET       0x100       /* EHCI operational regs offset */

/* EHCI Capability Registers (offset from base) */
#define EHCI_CAP_CAPLENGTH          0x00    /* 1 byte: length of cap regs */
#define EHCI_CAP_HCIVERSION         0x02    /* 2 bytes: EHCI version */
#define EHCI_CAP_HCSPARAMS          0x04    /* Structural parameters */
#define EHCI_CAP_HCCPARAMS          0x08    /* Capability parameters */

/* EHCI Operational Registers (offset from base + CAPLENGTH) */
#define EHCI_OP_USBCMD              0x00    /* USB Command */
#define EHCI_OP_USBSTS              0x04    /* USB Status */
#define EHCI_OP_USBINTR             0x08    /* USB Interrupt Enable */
#define EHCI_OP_FRINDEX             0x0C    /* Frame Index */
#define EHCI_OP_CTRLDSSEGMENT       0x10    /* 4G Segment Selector */
#define EHCI_OP_PERIODICLISTBASE    0x14    /* Periodic Frame List Base */
#define EHCI_OP_ASYNCLISTADDR       0x18    /* Async List Address (QH) */
#define EHCI_OP_CONFIGFLAG          0x40    /* Configured Flag */
#define EHCI_OP_PORTSC0             0x44    /* Port Status/Control */

/* USBCMD bits */
#define EHCI_CMD_RUN                (1 << 0)
#define EHCI_CMD_HCRESET            (1 << 1)
#define EHCI_CMD_PERIODIC_EN        (1 << 4)
#define EHCI_CMD_ASYNC_EN           (1 << 5)
#define EHCI_CMD_INT_THRESHOLD(n)   (((n) & 0xFF) << 16)

/* USBSTS bits */
#define EHCI_STS_USBINT             (1 << 0)
#define EHCI_STS_USBERRINT          (1 << 1)
#define EHCI_STS_PORT_CHANGE        (1 << 2)
#define EHCI_STS_FRAME_ROLLOVER     (1 << 3)
#define EHCI_STS_HOST_ERROR         (1 << 4)
#define EHCI_STS_ASYNC_ADVANCE      (1 << 5)
#define EHCI_STS_HALTED             (1 << 12)
#define EHCI_STS_RECLAMATION        (1 << 13)
#define EHCI_STS_PERIODIC_ACTIVE    (1 << 14)
#define EHCI_STS_ASYNC_ACTIVE       (1 << 15)

/* PORTSC bits */
#define EHCI_PORTSC_CONNECTED       (1 << 0)
#define EHCI_PORTSC_CONNECT_CHANGE  (1 << 1)
#define EHCI_PORTSC_ENABLED         (1 << 2)
#define EHCI_PORTSC_ENABLE_CHANGE   (1 << 3)
#define EHCI_PORTSC_OVERCURRENT     (1 << 4)
#define EHCI_PORTSC_OC_CHANGE       (1 << 5)
#define EHCI_PORTSC_RESUME          (1 << 6)
#define EHCI_PORTSC_SUSPENDED       (1 << 7)
#define EHCI_PORTSC_PORT_RESET      (1 << 8)
#define EHCI_PORTSC_LINE_STATUS     (3 << 10)
#define EHCI_PORTSC_PORT_POWER      (1 << 12)
#define EHCI_PORTSC_PORT_OWNER      (1 << 13) /* 1=companion, 0=EHCI */
#define EHCI_PORTSC_WRITE_CLEAR     (EHCI_PORTSC_CONNECT_CHANGE | \
                                     EHCI_PORTSC_ENABLE_CHANGE | \
                                     EHCI_PORTSC_OC_CHANGE)

/* =========================================================================
 * USB STANDARD DESCRIPTORS (defined locally to avoid SDK dependency)
 * ========================================================================= */

#pragma pack(push, 1)

typedef struct {
    uint8_t     bLength;
    uint8_t     bDescriptorType;
    uint16_t    bcdUSB;
    uint8_t     bDeviceClass;
    uint8_t     bDeviceSubClass;
    uint8_t     bDeviceProtocol;
    uint8_t     bMaxPacketSize0;
    uint16_t    idVendor;
    uint16_t    idProduct;
    uint16_t    bcdDevice;
    uint8_t     iManufacturer;
    uint8_t     iProduct;
    uint8_t     iSerialNumber;
    uint8_t     bNumConfigurations;
} USB_DEVICE_DESC;

typedef struct {
    uint8_t     bLength;
    uint8_t     bDescriptorType;
    uint16_t    wTotalLength;
    uint8_t     bNumInterfaces;
    uint8_t     bConfigurationValue;
    uint8_t     iConfiguration;
    uint8_t     bmAttributes;
    uint8_t     bMaxPower;
} USB_CONFIG_DESC;

typedef struct {
    uint8_t     bLength;
    uint8_t     bDescriptorType;
    uint8_t     bInterfaceNumber;
    uint8_t     bAlternateSetting;
    uint8_t     bNumEndpoints;
    uint8_t     bInterfaceClass;
    uint8_t     bInterfaceSubClass;
    uint8_t     bInterfaceProtocol;
    uint8_t     iInterface;
} USB_INTERFACE_DESC;

typedef struct {
    uint8_t     bLength;
    uint8_t     bDescriptorType;
    uint8_t     bEndpointAddress;
    uint8_t     bmAttributes;
    uint16_t    wMaxPacketSize;
    uint8_t     bInterval;
} USB_ENDPOINT_DESC;

/* USB Setup Packet (8 bytes, used for all control transfers) */
typedef struct {
    uint8_t     bmRequestType;
    uint8_t     bRequest;
    uint16_t    wValue;
    uint16_t    wIndex;
    uint16_t    wLength;
} USB_SETUP_PACKET;

#pragma pack(pop)

/* Descriptor type codes */
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIGURATION  2
#define USB_DESC_STRING         3
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5

/* Standard request codes */
#define USB_REQ_GET_STATUS      0
#define USB_REQ_CLEAR_FEATURE   1
#define USB_REQ_SET_FEATURE     3
#define USB_REQ_SET_ADDRESS     5
#define USB_REQ_GET_DESCRIPTOR  6
#define USB_REQ_SET_CONFIG      9

/* Endpoint direction mask */
#define USB_EP_DIR_IN           0x80
#define USB_EP_DIR_OUT          0x00

/* Transfer type mask (bmAttributes bits 0-1) */
#define USB_EP_TYPE_CONTROL     0
#define USB_EP_TYPE_ISOC        1
#define USB_EP_TYPE_BULK        2
#define USB_EP_TYPE_INTERRUPT   3
#define USB_EP_TYPE_MASK        0x03

/* =========================================================================
 * AOA v2.0 PROTOCOL CONSTANTS
 * ========================================================================= */

/* AOA vendor requests */
#define AOA_REQ_GET_PROTOCOL    51
#define AOA_REQ_SEND_STRING     52
#define AOA_REQ_START           53
#define AOA_REQ_SET_AUDIO_MODE  58  /* AOA v2.0: audio routing */

/* AOA string indices */
#define AOA_STRING_MANUFACTURER 0
#define AOA_STRING_MODEL        1
#define AOA_STRING_DESCRIPTION  2
#define AOA_STRING_VERSION      3
#define AOA_STRING_URI          4
#define AOA_STRING_SERIAL       5

/* Google VID and AOA PIDs */
#define GOOGLE_VID              0x18D1
#define AOA_PID_ACCESSORY       0x2D00
#define AOA_PID_ACCESSORY_ADB   0x2D01
#define AOA_PID_AUDIO           0x2D02
#define AOA_PID_AUDIO_ADB       0x2D03
#define AOA_PID_ACC_AUDIO       0x2D04
#define AOA_PID_ACC_AUDIO_ADB   0x2D05

/* =========================================================================
 * HCD IOCTL CODES (for direct HCD stream driver communication)
 *
 * These are reverse-engineered / inferred from WinCE 6.0 Platform Builder
 * USB host controller sources. The exact values may differ per BSP.
 * We probe multiple known IOCTL code ranges.
 * ========================================================================= */

#define IOCTL_HCD_GET_FRAME_NUMBER  \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x200, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_GET_FRAME_LENGTH  \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x201, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_ISSUE_TRANSFER    \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x210, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_OPEN_PIPE         \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x220, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_CLOSE_PIPE        \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x221, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_RESET_PORT        \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x230, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_DISABLE_PORT      \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x231, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HCD_SUSPEND_PORT      \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x232, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* =========================================================================
 * TRANSFER REQUEST STRUCTURE
 *
 * Passed to IOCTL_HCD_ISSUE_TRANSFER via DeviceIoControl.
 * Represents a single USB transfer request (control, bulk, etc.)
 * ========================================================================= */

#pragma pack(push, 1)
typedef struct {
    uint32_t    dwTransferType;     /* 0=control, 2=bulk, 3=interrupt */
    uint32_t    dwEndpoint;         /* Endpoint address */
    uint32_t    dwDirection;        /* 0=OUT, 1=IN */
    uint32_t    dwBufferSize;       /* Data buffer size */
    uint32_t    dwBytesTransferred; /* Actual bytes (output) */
    uint32_t    dwTimeout;          /* Timeout in ms */
    uint32_t    dwStatus;           /* Transfer status (output) */
    uint32_t    dwDeviceAddress;    /* USB device address (1-127) */
    uint32_t    dwMaxPacketSize;    /* Max packet for this endpoint */
    uint8_t     setupPacket[8];     /* For control transfers only */
    /* Data buffer follows this structure in the IOCTL buffer */
} HCD_TRANSFER_REQUEST;
#pragma pack(pop)

/* Transfer types */
#define HCD_XFER_CONTROL        0
#define HCD_XFER_ISOCHRONOUS    1
#define HCD_XFER_BULK           2
#define HCD_XFER_INTERRUPT      3

/* Transfer status codes */
#define HCD_STATUS_SUCCESS      0
#define HCD_STATUS_STALL        1
#define HCD_STATUS_TIMEOUT      2
#define HCD_STATUS_ERROR        3
#define HCD_STATUS_NAK          4

/* =========================================================================
 * ACCESS METHOD ENUMERATION
 * ========================================================================= */

typedef enum {
    USB_ACCESS_NONE = 0,
    USB_ACCESS_HCD_STREAM,      /* Via CreateFile("HCD1:") */
    USB_ACCESS_EHCI_DIRECT,     /* Via VirtualCopy to EHCI MMIO */
} usb_access_method_t;

/* =========================================================================
 * ENDPOINT STATE
 * ========================================================================= */

typedef struct {
    uint8_t     bAddress;           /* Endpoint address (including direction) */
    uint8_t     bType;              /* Transfer type (bulk/interrupt/etc.) */
    uint16_t    wMaxPacketSize;     /* Max packet size */
    uint8_t     bInterval;          /* Polling interval (interrupt only) */
    uint32_t    dwPipeHandle;       /* HCD pipe handle (if using HCD method) */
} usb_endpoint_state_t;

/* =========================================================================
 * USB DEVICE STATE (enriched from mn1_usb_conn_t)
 * ========================================================================= */

typedef struct {
    /* Access method */
    usb_access_method_t     accessMethod;

    /* HCD stream driver handle (method: HCD_STREAM) */
    HANDLE                  hHCD;

    /* EHCI register mapping (method: EHCI_DIRECT) */
    volatile uint32_t*      pEhciCap;       /* Capability registers */
    volatile uint32_t*      pEhciOp;        /* Operational registers */
    uint32_t                dwEhciPhysBase;
    void*                   pEhciVirtBase;
    uint32_t                dwCapLength;

    /* USB device state */
    uint8_t                 bDeviceAddress;  /* Assigned USB address (1-127) */
    uint16_t                wVendorId;
    uint16_t                wProductId;
    uint8_t                 bMaxPacketSize0; /* EP0 max packet */
    uint8_t                 bNumEndpoints;

    /* Discovered endpoints (after configuration) */
    usb_endpoint_state_t    epBulkIn;
    usb_endpoint_state_t    epBulkOut;
    usb_endpoint_state_t    epControl;      /* EP0 is always control */

    /* Connection state */
    mn1_bool_t              bDevicePresent;
    mn1_bool_t              bConfigured;
    mn1_bool_t              bAOAMode;
    uint16_t                wAOAProtocolVer;

    /* Worker thread handles */
    HANDLE                  hVideoInThread;
    HANDLE                  hTouchOutThread;
    HANDLE                  hVideoInEvent;   /* Signal: data available */
    HANDLE                  hTouchOutEvent;  /* Signal: send touch event */
    volatile mn1_bool_t     bThreadsRunning;

    /* Double-buffered video receive */
    uint8_t*                pVideoRxBuf[2];
    uint32_t                dwVideoRxLen[2];
    volatile int            nActiveVideoBuf;  /* 0 or 1 */
    CRITICAL_SECTION        csVideoSwap;

    /* Touch event queue */
    mn1_touch_event_t       pendingTouch;
    volatile mn1_bool_t     bTouchPending;

    /* Statistics */
    volatile uint32_t       dwTotalBytesIn;
    volatile uint32_t       dwTotalBytesOut;
    volatile uint32_t       dwTransferErrors;
    volatile uint32_t       dwNAKRetries;

} usb_device_state_t;

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

/*
 * Phase 1: Initialization
 * - Kill MgrUSB.exe
 * - Probe for USB host access (HCD stream or EHCI direct)
 * - Initialize EHCI controller if using direct access
 */
mn1_result_t usb_handler_init(usb_device_state_t* pDev);

/*
 * Phase 2: Device Enumeration
 * - Wait for device connection (poll PORTSC or HCD)
 * - Reset port
 * - Read device descriptor (GET_DESCRIPTOR)
 * - Assign address (SET_ADDRESS)
 * - Read full configuration descriptor
 * - Set configuration (SET_CONFIGURATION)
 * - Discover bulk endpoints
 */
mn1_result_t usb_handler_enumerate(usb_device_state_t* pDev, uint32_t dwTimeoutMs);

/*
 * Phase 3: AOA v2.0 Handshake
 * - Check if device is already in AOA mode (check VID:PID)
 * - If not: GET_PROTOCOL -> SEND_STRING x6 -> START
 * - Wait for re-enumeration
 * - Re-enumerate the AOA device
 * - Find bulk IN/OUT endpoints
 */
mn1_result_t usb_handler_aoa_handshake(usb_device_state_t* pDev);

/*
 * Phase 4: Start Worker Threads
 * - Video IN thread (reads H.264/data from phone)
 * - Touch OUT thread (sends touch events to phone)
 * Both run at elevated priority for MIPS CPU compensation.
 */
mn1_result_t usb_handler_start_workers(usb_device_state_t* pDev);

/*
 * Raw Transfer Primitives
 * Used by AOA handshake and available for higher layers.
 */
mn1_result_t usb_handler_control_transfer(
    usb_device_state_t* pDev,
    const USB_SETUP_PACKET* pSetup,
    uint8_t*            pData,
    uint32_t            dwMaxLen,
    uint32_t*           pdwActualLen,
    uint32_t            dwTimeoutMs
);

mn1_result_t usb_handler_bulk_read(
    usb_device_state_t* pDev,
    uint8_t*            pBuffer,
    uint32_t            dwMaxBytes,
    uint32_t*           pdwActual,
    uint32_t            dwTimeoutMs
);

mn1_result_t usb_handler_bulk_write(
    usb_device_state_t* pDev,
    const uint8_t*      pBuffer,
    uint32_t            dwBytes,
    uint32_t*           pdwActual,
    uint32_t            dwTimeoutMs
);

/*
 * Queue a touch event for the OUT worker thread.
 */
void usb_handler_queue_touch(
    usb_device_state_t*     pDev,
    const mn1_touch_event_t* pEvent
);

/*
 * Get the latest received video buffer (swap and return).
 * Returns pointer to the inactive buffer (safe to read).
 * *pdwLen receives the data length.
 */
const uint8_t* usb_handler_get_video_frame(
    usb_device_state_t* pDev,
    uint32_t*           pdwLen
);

/*
 * Shutdown everything: stop threads, close handles, unmap memory.
 */
void usb_handler_shutdown(usb_device_state_t* pDev);

/*
 * Compatibility wrapper: populates a legacy mn1_usb_conn_t from
 * the new usb_device_state_t for modules that still use the old API.
 */
void usb_handler_get_legacy_conn(
    const usb_device_state_t* pDev,
    mn1_usb_conn_t*           pConn
);

#ifdef __cplusplus
}
#endif

#endif /* MN1AA_USB_HANDLER_H */
