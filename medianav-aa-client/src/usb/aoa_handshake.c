/*
 * aoa_handshake.c - Android Open Accessory Protocol Implementation
 *
 * Full AOA handshake for WinCE 6.0.
 *
 * PROTOCOL REFERENCE (from Google source.android.com):
 *
 * Step 1: GET_PROTOCOL
 *   bmRequestType = 0xC0 (Device-to-Host, Vendor, Device)
 *   bRequest      = 51
 *   wValue        = 0
 *   wIndex        = 0
 *   wLength       = 2
 *   Response: uint16_t protocol version (LE)
 *
 * Step 2: SEND_STRING (x6, indices 0-5)
 *   bmRequestType = 0x40 (Host-to-Device, Vendor, Device)
 *   bRequest      = 52
 *   wValue        = 0
 *   wIndex        = string_index (0=manufacturer, 1=model, ...)
 *   wLength       = strlen + 1 (null-terminated)
 *
 * Step 3: START
 *   bmRequestType = 0x40
 *   bRequest      = 53
 *   wValue        = 0
 *   wIndex        = 0
 *   wLength       = 0
 *
 * Step 4: Device disconnects and re-enumerates with:
 *   VID = 0x18D1, PID = 0x2D00 (accessory) or 0x2D01 (accessory+ADB)
 */

#include "aoa_handshake.h"
#include "util/debug_log.h"

/* Identity strings sent to the Android device */
static const char* s_aoaStrings[] = {
    MN1_AOA_MANUFACTURER,   /* Index 0: Manufacturer */
    MN1_AOA_MODEL,          /* Index 1: Model name */
    MN1_AOA_DESCRIPTION,    /* Index 2: Description */
    MN1_AOA_VERSION,        /* Index 3: Version (CRITICAL: must not be empty!) */
    MN1_AOA_URI,            /* Index 4: URI (optional) */
    MN1_AOA_SERIAL,         /* Index 5: Serial (optional) */
};

#define AOA_STRING_COUNT    6

/* =========================================================================
 * Step 1: Check Protocol
 * ========================================================================= */

mn1_result_t mn1_aoa_check_protocol(
    mn1_usb_conn_t*          pConn,
    aoa_protocol_version_t*  pVersion)
{
    uint8_t response[2] = {0, 0};
    uint32_t dwActual = 0;
    mn1_result_t result;

    MN1_LOG_INFO(L"AOA: Checking protocol support...");

    /* GET_PROTOCOL: request 51, device-to-host vendor transfer */
    result = mn1_usb_vendor_in(
        pConn,
        MN1_AOA_GET_PROTOCOL,   /* bRequest = 51 */
        0,                       /* wValue   = 0 */
        0,                       /* wIndex   = 0 */
        response,                /* pData */
        2,                       /* wLength  = 2 */
        &dwActual
    );

    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"AOA: GET_PROTOCOL failed (device may not support AOA)");
        *pVersion = AOA_PROTOCOL_NONE;
        return MN1_ERR_USB_AOA_UNSUPPORTED;
    }

    /* Protocol version is 16-bit LE */
    uint16_t version = (uint16_t)(response[0] | (response[1] << 8));
    MN1_LOG_INFO(L"AOA: Protocol version = %d", version);

    if (version == 0) {
        MN1_LOG_ERROR(L"AOA: Device reports protocol version 0 (unsupported)");
        *pVersion = AOA_PROTOCOL_NONE;
        return MN1_ERR_USB_AOA_UNSUPPORTED;
    }

    *pVersion = (version >= 2) ? AOA_PROTOCOL_V2 : AOA_PROTOCOL_V1;
    return MN1_OK;
}

/* =========================================================================
 * Step 2: Send Identity Strings
 * ========================================================================= */

static mn1_result_t aoa_send_strings(mn1_usb_conn_t* pConn)
{
    mn1_result_t result;
    int i;

    MN1_LOG_INFO(L"AOA: Sending identity strings...");

    for (i = 0; i < AOA_STRING_COUNT; i++) {
        const char* str = s_aoaStrings[i];
        uint16_t len = 0;

        /* Calculate string length + null terminator */
        while (str[len] != '\0')
            len++;
        len++; /* Include null terminator */

        MN1_LOG_INFO(L"AOA:   String[%d] = \"%hs\" (%d bytes)", i, str, len);

        /*
         * SEND_STRING: request 52
         *   wValue = 0
         *   wIndex = string index (0-5)
         *   data   = null-terminated ASCII string
         */
        result = mn1_usb_vendor_out(
            pConn,
            MN1_AOA_SEND_STRING,    /* bRequest = 52 */
            0,                       /* wValue   = 0 */
            (uint16_t)i,            /* wIndex   = string index */
            (const uint8_t*)str,    /* data */
            len                      /* length including null */
        );

        if (result != MN1_OK) {
            MN1_LOG_ERROR(L"AOA: Failed to send string[%d]", i);
            return MN1_ERR_USB_AOA_HANDSHAKE;
        }

        /* Small delay between strings for device processing.
         * Some Android devices need time between control transfers.
         * 10ms is conservative but safe. */
        Sleep(10);
    }

    return MN1_OK;
}

/* =========================================================================
 * Step 3: Start Accessory Mode
 * ========================================================================= */

static mn1_result_t aoa_start_accessory(mn1_usb_conn_t* pConn)
{
    MN1_LOG_INFO(L"AOA: Sending START command...");

    /*
     * START: request 53
     * After this, the device will disconnect and re-enumerate
     * with the AOA VID:PID.
     */
    return mn1_usb_vendor_out(
        pConn,
        MN1_AOA_START,  /* bRequest = 53 */
        0,               /* wValue   = 0 */
        0,               /* wIndex   = 0 */
        NULL,            /* no data */
        0                /* wLength  = 0 */
    );
}

/* =========================================================================
 * Step 4: Wait for Re-enumeration
 * ========================================================================= */

static mn1_result_t aoa_wait_reenumeration(mn1_usb_conn_t* pConn)
{
    mn1_result_t result;
    uint32_t dwStartTick;
    uint32_t dwTimeout = 10000; /* 10 seconds for re-enumeration */

    MN1_LOG_INFO(L"AOA: Waiting for device re-enumeration...");

    /*
     * After START, the Android device:
     * 1. Disconnects from USB bus
     * 2. Switches internal USB mode to AOA
     * 3. Re-enumerates with VID=0x18D1, PID=0x2D00/0x2D01
     *
     * We need to:
     * 1. Close our current device handle (it's now invalid)
     * 2. Wait for the new device to appear
     * 3. Open and claim the new device
     * 4. Find bulk IN/OUT endpoints
     */

    /* Close the old device handle */
    mn1_usb_close(pConn);

    /* Wait a moment for USB disconnect/reconnect cycle */
    Sleep(2000);

    /* Scan for the AOA device */
    dwStartTick = mn1_get_tick_ms();

    while ((mn1_get_tick_ms() - dwStartTick) < dwTimeout) {

        /* Try to find and open the AOA device */
        result = mn1_usb_wait_device(1000, pConn);

        if (result == MN1_OK) {
            /*
             * Device found - verify it's the AOA device.
             *
             * We need to check VID:PID.
             * On WinCE, we can read the device descriptor via
             * DeviceIoControl with IOCTL_USB_GET_DEVICE_DESCRIPTOR.
             *
             * For our prototype, we assume if a device appears
             * shortly after START, it's our AOA device.
             */
            MN1_LOG_INFO(L"AOA: Device re-enumerated successfully");

            /*
             * Configure bulk endpoints.
             *
             * AOA interface 0 has exactly 2 bulk endpoints:
             * - Bulk IN (device -> host): for receiving data from phone
             * - Bulk OUT (host -> device): for sending data to phone
             *
             * Max packet size: 512 bytes for USB 2.0 High Speed
             */
            pConn->wMaxPacketIn  = 512;
            pConn->wMaxPacketOut = 512;

            /* The bulk pipes are typically opened as sub-devices
             * or via interface claim. On WinCE, this depends on
             * the USB client driver implementation.
             *
             * For a generic driver, endpoints are accessed through
             * the same device handle with endpoint-specific IOCTLs. */
            pConn->hBulkIn  = pConn->hUSBDevice;  /* Shared handle */
            pConn->hBulkOut = pConn->hUSBDevice;   /* Shared handle */

            return MN1_OK;
        }

        Sleep(500); /* Wait before retry */
    }

    MN1_LOG_ERROR(L"AOA: Re-enumeration timeout (device did not reappear)");
    return MN1_ERR_USB_AOA_HANDSHAKE;
}

/* =========================================================================
 * COMPLETE HANDSHAKE
 * ========================================================================= */

mn1_result_t mn1_aoa_handshake(mn1_usb_conn_t* pConn)
{
    mn1_result_t result;
    aoa_protocol_version_t version;

    MN1_LOG_INFO(L"========================================");
    MN1_LOG_INFO(L"  AOA Handshake Starting");
    MN1_LOG_INFO(L"========================================");

    /* Step 1: Check protocol */
    result = mn1_aoa_check_protocol(pConn, &version);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"AOA: Device does not support AOA protocol");
        return result;
    }

    MN1_LOG_INFO(L"AOA: Protocol v%d supported", (int)version);

    /* Step 2: Send identity strings */
    result = aoa_send_strings(pConn);
    if (result != MN1_OK)
        return result;

    /* Step 3: Start accessory mode */
    result = aoa_start_accessory(pConn);
    if (result != MN1_OK)
        return result;

    /* Step 4: Wait for re-enumeration */
    result = aoa_wait_reenumeration(pConn);
    if (result != MN1_OK)
        return result;

    MN1_LOG_INFO(L"========================================");
    MN1_LOG_INFO(L"  AOA Handshake COMPLETE");
    MN1_LOG_INFO(L"  Bulk IN ready  (ep=%p)", pConn->hBulkIn);
    MN1_LOG_INFO(L"  Bulk OUT ready (ep=%p)", pConn->hBulkOut);
    MN1_LOG_INFO(L"========================================");

    return MN1_OK;
}
