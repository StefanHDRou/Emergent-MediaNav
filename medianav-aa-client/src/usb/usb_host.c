/*
 * usb_host.c - WinCE 6.0 USB Host Implementation
 *
 * Implements USB Host operations using the WinCE USBD API.
 * Target: Alchemy Au1320 USB 2.0/OTG controller.
 *
 * WinCE USB Host Architecture:
 *   Application -> USBD.dll -> HCD (Host Controller Driver) -> Hardware
 *
 * We use the stream driver model: our USB client driver is loaded by USBD
 * when a matching device is connected. However, for simplicity in this
 * standalone .exe, we use the FindFirstDevice/DeviceIoControl approach
 * to communicate with the USB stack directly.
 *
 * ALTERNATIVE APPROACH (simpler, recommended for prototyping):
 *   Use the ActiveSync/RNDIS USB interface or load a generic USB
 *   client driver that exposes bulk endpoints as a stream device.
 */

#include "usb_host.h"
#include "util/debug_log.h"

/* WinCE USB headers (from Platform Builder SDK) */
/* These are part of the CE 6.0 SDK and define the USBD interface */
#ifdef UNDER_CE
#include <usbtypes.h>       /* USB_DEVICE_DESCRIPTOR, etc. */
#include <usbdi.h>          /* USBD interface functions */
#else
/* Stub definitions for cross-development/testing on desktop */
typedef void* USB_HANDLE;
typedef void* USB_PIPE;
#endif

/* =========================================================================
 * Au1320 OTG Register Map (from Alchemy Au1xxx family documentation)
 *
 * The Au1320's USB controller is at a specific physical address.
 * These addresses may need adjustment based on the actual MediaNav BSP.
 * ========================================================================= */

/* Au1320 USB OTG controller base (typical for Au1xxx family) */
#define AU1320_USB_BASE             0x14020000

/* OTG Control register offset */
#define AU1320_USB_OTG_CTRL         0x00
#define AU1320_USB_OTG_STAT         0x04
#define AU1320_USB_HOST_CTRL        0x08

/* OTG Control bits */
#define AU1320_OTG_FORCE_HOST       (1 << 0)
#define AU1320_OTG_FORCE_DEVICE     (1 << 1)
#define AU1320_OTG_VBUS_ON          (1 << 2)
#define AU1320_OTG_PORT_POWER       (1 << 3)

/* =========================================================================
 * INTERNAL STATE
 * ========================================================================= */

/* Device notification event */
static HANDLE s_hDeviceEvent = NULL;

/* Registry-based device detection path */
static const WCHAR* USB_DEVICE_SEARCH_KEY =
    L"Drivers\\USB\\ClientDrivers\\MN1AA";

/* =========================================================================
 * USB OTG MODE SWITCHING
 * ========================================================================= */

mn1_result_t mn1_usb_switch_to_host_mode(void)
{
    /*
     * Strategy 1: Direct register access via VirtualAlloc/VirtualCopy
     *
     * On WinCE 6.0, user-mode processes can map physical memory using
     * VirtualAlloc + VirtualCopy. This is how we access the Au1320's
     * OTG control registers to force USB Host mode.
     *
     * WARNING: Wrong addresses = hard crash. Verify against your BSP.
     */
    volatile uint32_t* pOTGRegs = NULL;
    void* pVirtBase = NULL;

    MN1_LOG_INFO(L"Attempting USB OTG Host mode switch...");

    /* Allocate virtual address space */
    pVirtBase = VirtualAlloc(0, 4096, MEM_RESERVE, PAGE_READWRITE);
    if (!pVirtBase) {
        MN1_LOG_ERROR(L"VirtualAlloc failed for OTG regs: %d", GetLastError());
        goto try_registry;
    }

    /* Map physical address to virtual space
     * Note: VirtualCopy on CE 6.0 requires the physical address
     * to be shifted right by 8 bits (PAGE_PHYSICAL flag) */
    if (!VirtualCopy(
            pVirtBase,
            (void*)(AU1320_USB_BASE >> 8),
            4096,
            PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL))
    {
        MN1_LOG_WARN(L"VirtualCopy failed for 0x%08X: %d",
                     AU1320_USB_BASE, GetLastError());
        VirtualFree(pVirtBase, 0, MEM_RELEASE);
        goto try_registry;
    }

    pOTGRegs = (volatile uint32_t*)pVirtBase;

    /* Read current OTG status */
    MN1_LOG_INFO(L"OTG_CTRL=0x%08X, OTG_STAT=0x%08X",
                 pOTGRegs[AU1320_USB_OTG_CTRL / 4],
                 pOTGRegs[AU1320_USB_OTG_STAT / 4]);

    /* Force Host mode: Set FORCE_HOST bit, clear FORCE_DEVICE, enable VBUS */
    pOTGRegs[AU1320_USB_OTG_CTRL / 4] =
        AU1320_OTG_FORCE_HOST |
        AU1320_OTG_VBUS_ON |
        AU1320_OTG_PORT_POWER;

    /* Wait for mode switch to take effect */
    Sleep(500);

    /* Verify host mode */
    uint32_t stat = pOTGRegs[AU1320_USB_OTG_STAT / 4];
    MN1_LOG_INFO(L"After switch: OTG_STAT=0x%08X", stat);

    VirtualFree(pVirtBase, 0, MEM_RELEASE);
    return MN1_OK;

try_registry:
    /*
     * Strategy 2: Registry-based USB role switching
     *
     * Some WinCE BSPs expose USB role via registry keys.
     * Try known locations used by various OTG drivers.
     */
    MN1_LOG_INFO(L"Trying registry-based USB role switch...");

    {
        HKEY hKey;
        DWORD dwMode = 1; /* 1 = Host mode */
        LONG lResult;

        /* Common registry paths for USB OTG mode */
        static const WCHAR* rgszKeys[] = {
            L"Drivers\\USB\\FunctionController",
            L"Drivers\\USB\\OTG",
            L"Comm\\USB\\OTGMode",
            NULL
        };

        for (int i = 0; rgszKeys[i] != NULL; i++) {
            lResult = RegOpenKeyEx(HKEY_LOCAL_MACHINE, rgszKeys[i],
                                   0, KEY_WRITE, &hKey);
            if (lResult == ERROR_SUCCESS) {
                lResult = RegSetValueEx(hKey, L"HostMode", 0, REG_DWORD,
                                       (BYTE*)&dwMode, sizeof(dwMode));
                RegCloseKey(hKey);
                if (lResult == ERROR_SUCCESS) {
                    MN1_LOG_INFO(L"Set HostMode=1 at %s", rgszKeys[i]);
                    Sleep(500);
                    return MN1_OK;
                }
            }
        }
    }

    MN1_LOG_ERROR(L"USB Host mode switch FAILED. Use OTG cable with ID pin grounded.");
    return MN1_ERR_GENERIC;
}

/* =========================================================================
 * USB INITIALIZATION
 * ========================================================================= */

mn1_result_t mn1_usb_init(void)
{
    mn1_result_t result;

    MN1_LOG_INFO(L"Initializing USB host stack...");

    /* Create device arrival event */
    s_hDeviceEvent = CreateEvent(NULL, FALSE, FALSE, L"MN1AA_USB_DEVICE");
    if (!s_hDeviceEvent) {
        MN1_LOG_ERROR(L"CreateEvent failed: %d", GetLastError());
        return MN1_ERR_USB_INIT;
    }

    /* Attempt OTG mode switch */
    result = mn1_usb_switch_to_host_mode();
    if (result != MN1_OK) {
        MN1_LOG_WARN(L"OTG switch failed - continuing (may work with OTG cable)");
        /* Don't fail here - the OTG cable may handle it */
    }

    MN1_LOG_INFO(L"USB init complete");
    return MN1_OK;
}

/* =========================================================================
 * DEVICE ENUMERATION
 * ========================================================================= */

mn1_result_t mn1_usb_wait_device(uint32_t dwTimeoutMs, mn1_usb_conn_t* pConn)
{
    HANDLE hSearch;
    DEVMGR_DEVICE_INFORMATION di;
    DWORD dwWaitResult;

    MN1_LOG_INFO(L"Scanning for USB devices...");

    memset(pConn, 0, sizeof(mn1_usb_conn_t));

    /*
     * On WinCE 6.0, USB devices appear as stream devices under
     * Drivers\USB\ClientDrivers. We scan for any connected USB device.
     *
     * The USBD stack exposes connected devices through the Device Manager.
     * We use FindFirstDevice with DEVICE_MATCH_KEY filter.
     */

    /* First, check if a device is already connected */
    memset(&di, 0, sizeof(di));
    di.dwSize = sizeof(di);

    /* Search for USB devices using generic search */
    GUID guidUSB = { 0xA5DCBF10L, 0x6530, 0x11D2,
        { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

    hSearch = FindFirstDevice(DeviceSearchByGuid,
                              &guidUSB,
                              &di);

    if (hSearch != INVALID_HANDLE_VALUE) {
        /* Device found immediately */
        FindClose(hSearch);
        MN1_LOG_INFO(L"Found USB device: %s", di.szDeviceName);

        /* Open the device */
        pConn->hUSBDevice = CreateFile(
            di.szDeviceName,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (pConn->hUSBDevice == INVALID_HANDLE_VALUE) {
            MN1_LOG_ERROR(L"Failed to open USB device: %d", GetLastError());
            return MN1_ERR_USB_NO_DEVICE;
        }

        pConn->bConnected = TRUE;
        return MN1_OK;
    }

    /* No device found, wait for connection */
    if (dwTimeoutMs == 0)
        dwTimeoutMs = INFINITE;

    MN1_LOG_INFO(L"No device found, waiting %d ms...", dwTimeoutMs);
    dwWaitResult = WaitForSingleObject(s_hDeviceEvent, dwTimeoutMs);

    if (dwWaitResult == WAIT_TIMEOUT) {
        MN1_LOG_WARN(L"Device wait timed out");
        return MN1_ERR_USB_NO_DEVICE;
    }

    /* Re-scan after event */
    hSearch = FindFirstDevice(DeviceSearchByGuid, &guidUSB, &di);
    if (hSearch != INVALID_HANDLE_VALUE) {
        FindClose(hSearch);

        pConn->hUSBDevice = CreateFile(
            di.szDeviceName,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (pConn->hUSBDevice != INVALID_HANDLE_VALUE) {
            pConn->bConnected = TRUE;
            return MN1_OK;
        }
    }

    return MN1_ERR_USB_NO_DEVICE;
}

/* =========================================================================
 * BULK TRANSFERS
 * ========================================================================= */

mn1_result_t mn1_usb_bulk_read(
    mn1_usb_conn_t* pConn,
    uint8_t*        pBuffer,
    uint32_t        dwMaxBytes,
    uint32_t*       pdwActual,
    uint32_t        dwTimeoutMs)
{
    BOOL bResult;
    DWORD dwBytesRead = 0;

    if (!pConn->bConnected || pConn->hBulkIn == NULL)
        return MN1_ERR_USB_NO_DEVICE;

    /*
     * WinCE USBD bulk read via DeviceIoControl or ReadFile
     * depending on how the client driver exposes the pipe.
     *
     * For a generic USB client driver, use DeviceIoControl
     * with IOCTL_USB_BULK_OR_INTERRUPT_TRANSFER.
     *
     * For our purposes, if using a stream-based driver,
     * ReadFile on the bulk IN pipe handle works.
     */
    bResult = ReadFile(
        pConn->hBulkIn,
        pBuffer,
        dwMaxBytes,
        &dwBytesRead,
        NULL  /* No overlapped I/O */
    );

    if (pdwActual)
        *pdwActual = dwBytesRead;

    if (!bResult) {
        DWORD dwErr = GetLastError();
        if (dwErr == ERROR_TIMEOUT)
            return MN1_ERR_TIMEOUT;
        MN1_LOG_ERROR(L"USB bulk read failed: %d", dwErr);
        return MN1_ERR_USB_TRANSFER;
    }

    return MN1_OK;
}

mn1_result_t mn1_usb_bulk_write(
    mn1_usb_conn_t* pConn,
    const uint8_t*  pBuffer,
    uint32_t        dwBytes,
    uint32_t*       pdwActual,
    uint32_t        dwTimeoutMs)
{
    BOOL bResult;
    DWORD dwBytesWritten = 0;

    if (!pConn->bConnected || pConn->hBulkOut == NULL)
        return MN1_ERR_USB_NO_DEVICE;

    bResult = WriteFile(
        pConn->hBulkOut,
        pBuffer,
        dwBytes,
        &dwBytesWritten,
        NULL
    );

    if (pdwActual)
        *pdwActual = dwBytesWritten;

    if (!bResult) {
        MN1_LOG_ERROR(L"USB bulk write failed: %d", GetLastError());
        return MN1_ERR_USB_TRANSFER;
    }

    return MN1_OK;
}

/* =========================================================================
 * VENDOR/CONTROL TRANSFERS
 * ========================================================================= */

/*
 * Control transfer using DeviceIoControl.
 * On WinCE, vendor requests go through the USBD default control pipe.
 */

/* IOCTL code for USB vendor transfer (WinCE specific) */
#define IOCTL_USB_VENDOR_REQUEST    \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x100, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Vendor request structure for DeviceIoControl */
#pragma pack(push, 1)
typedef struct {
    uint8_t     bmRequestType;
    uint8_t     bRequest;
    uint16_t    wValue;
    uint16_t    wIndex;
    uint16_t    wLength;
} USB_VENDOR_REQUEST_T;
#pragma pack(pop)

mn1_result_t mn1_usb_vendor_out(
    mn1_usb_conn_t* pConn,
    uint8_t         bRequest,
    uint16_t        wValue,
    uint16_t        wIndex,
    const uint8_t*  pData,
    uint16_t        wLength)
{
    USB_VENDOR_REQUEST_T req;
    DWORD dwBytesReturned;
    uint8_t buffer[256 + sizeof(USB_VENDOR_REQUEST_T)];
    BOOL bResult;

    if (!pConn->bConnected)
        return MN1_ERR_USB_NO_DEVICE;

    /* Build the request structure */
    req.bmRequestType = 0x40; /* USB_DIR_HOST_TO_DEVICE | USB_TYPE_VENDOR */
    req.bRequest      = bRequest;
    req.wValue        = wValue;
    req.wIndex        = wIndex;
    req.wLength       = wLength;

    /* Copy request header + data into buffer */
    memcpy(buffer, &req, sizeof(req));
    if (pData && wLength > 0)
        memcpy(buffer + sizeof(req), pData, wLength);

    bResult = DeviceIoControl(
        pConn->hUSBDevice,
        IOCTL_USB_VENDOR_REQUEST,
        buffer,
        sizeof(req) + wLength,
        NULL,
        0,
        &dwBytesReturned,
        NULL
    );

    if (!bResult) {
        MN1_LOG_ERROR(L"USB vendor OUT req=%d failed: %d",
                      bRequest, GetLastError());
        return MN1_ERR_USB_TRANSFER;
    }

    return MN1_OK;
}

mn1_result_t mn1_usb_vendor_in(
    mn1_usb_conn_t* pConn,
    uint8_t         bRequest,
    uint16_t        wValue,
    uint16_t        wIndex,
    uint8_t*        pData,
    uint16_t        wLength,
    uint32_t*       pdwActual)
{
    USB_VENDOR_REQUEST_T req;
    DWORD dwBytesReturned = 0;
    BOOL bResult;

    if (!pConn->bConnected)
        return MN1_ERR_USB_NO_DEVICE;

    req.bmRequestType = 0xC0; /* USB_DIR_DEVICE_TO_HOST | USB_TYPE_VENDOR */
    req.bRequest      = bRequest;
    req.wValue        = wValue;
    req.wIndex        = wIndex;
    req.wLength       = wLength;

    bResult = DeviceIoControl(
        pConn->hUSBDevice,
        IOCTL_USB_VENDOR_REQUEST,
        &req,
        sizeof(req),
        pData,
        wLength,
        &dwBytesReturned,
        NULL
    );

    if (pdwActual)
        *pdwActual = dwBytesReturned;

    if (!bResult) {
        MN1_LOG_ERROR(L"USB vendor IN req=%d failed: %d",
                      bRequest, GetLastError());
        return MN1_ERR_USB_TRANSFER;
    }

    return MN1_OK;
}

/* =========================================================================
 * CLEANUP
 * ========================================================================= */

void mn1_usb_close(mn1_usb_conn_t* pConn)
{
    if (pConn->hBulkIn && pConn->hBulkIn != INVALID_HANDLE_VALUE)
        CloseHandle(pConn->hBulkIn);
    if (pConn->hBulkOut && pConn->hBulkOut != INVALID_HANDLE_VALUE)
        CloseHandle(pConn->hBulkOut);
    if (pConn->hUSBDevice && pConn->hUSBDevice != INVALID_HANDLE_VALUE)
        CloseHandle(pConn->hUSBDevice);

    pConn->hBulkIn = NULL;
    pConn->hBulkOut = NULL;
    pConn->hUSBDevice = NULL;
    pConn->bConnected = FALSE;
}

void mn1_usb_shutdown(void)
{
    if (s_hDeviceEvent) {
        CloseHandle(s_hDeviceEvent);
        s_hDeviceEvent = NULL;
    }
}

mn1_bool_t mn1_usb_is_aoa_device(const mn1_usb_conn_t* pConn)
{
    /*
     * After AOA handshake, the device re-enumerates with:
     * VID = 0x18D1 (Google), PID = 0x2D00 (AOA) or 0x2D01 (AOA+ADB)
     *
     * We check this by reading the device descriptor via DeviceIoControl.
     * For now, if the device is connected through our AOA flow,
     * we track this state via the handshake function.
     */
    (void)pConn;
    /* State tracked by aoa_handshake module */
    return FALSE;
}
