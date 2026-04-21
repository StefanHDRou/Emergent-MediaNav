/*
 * usb_handler.cpp - User-Mode USB Host Stack for MediaNav MN1
 *
 * COMPLETE REWRITE of the USB connectivity layer.
 *
 * This module bypasses the OS driver layer entirely because:
 *   1. No PnP manager exists in this stripped WinCE image
 *   2. No usbser.dll or generic USB client driver available
 *   3. MgrUSB.exe exclusively claims the port for Mass Storage
 *   4. Standard USBD client registration (RegisterClientDriverID)
 *      requires PnP dispatch which does not exist here
 *
 * Instead, we:
 *   A) Kill MgrUSB.exe to release the USB hardware
 *   B) Open HCD stream driver directly via CreateFile("HCD1:")
 *   C) If HCD stream is unavailable, fall back to direct EHCI
 *      register access via VirtualAlloc + VirtualCopy (MMIO)
 *   D) Perform all USB transactions (control, bulk) ourselves
 *   E) Implement AOA v2.0 with raw USB control transfers
 *   F) Manage bulk endpoints with dedicated high-priority threads
 *
 * TARGET: MIPS (Au1320), WinCE 6.0
 * NO x86/ARM intrinsics. Standard Win32/WinCE memory management only.
 *
 * BUILD: Compile as C++ (.cpp) for WinCE MIPS target.
 *        Link with: coredll.lib toolhelp.lib
 */

#include "usb/usb_handler.h"
#include "util/debug_log.h"

/* WinCE toolhelp for process enumeration (to kill MgrUSB.exe) */
#include <tlhelp32.h>

/* =========================================================================
 * VERBOSE LOGGING MACROS
 *
 * Every USB operation is logged at INFO level for debugging.
 * In release builds (MN1_LOG_LEVEL=0) these compile to nothing.
 * ========================================================================= */

#define USB_LOG_PHASE(phase) \
    MN1_LOG_INFO(L"==== USB HANDLER: %s ====", L##phase)

#define USB_LOG_HEX(label, buf, len) do { \
    if ((len) > 0 && (len) <= 64) { \
        WCHAR _hx[200]; int _hi = 0; \
        for (uint32_t _i = 0; _i < (len) && _hi < 190; _i++) \
            _hi += wsprintf(_hx + _hi, L"%02X ", ((const uint8_t*)(buf))[_i]); \
        MN1_LOG_INFO(L"  %s [%d]: %s", L##label, (len), _hx); \
    } \
} while(0)

/* =========================================================================
 * PHASE 0: KILL MgrUSB.exe
 *
 * MgrUSB.exe is a background MediaNav process that claims the USB port
 * exclusively for Mass Storage. It must be terminated before we can
 * access the USB hardware.
 *
 * We use the ToolHelp API (CreateToolhelp32Snapshot) to enumerate
 * processes and OpenProcess + TerminateProcess to kill it.
 * ========================================================================= */

static mn1_result_t kill_mgrusb(void)
{
    HANDLE hSnap;
    PROCESSENTRY32 pe;
    BOOL bFound = FALSE;
    int nKilled = 0;

    USB_LOG_PHASE("PHASE 0 - Kill MgrUSB.exe");

    /* Take a snapshot of all running processes */
    hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        MN1_LOG_ERROR(L"  CreateToolhelp32Snapshot failed: %d", GetLastError());
        MN1_LOG_WARN(L"  Continuing without killing MgrUSB (may fail later)");
        return MN1_OK; /* Non-fatal: maybe it's not running */
    }

    pe.dwSize = sizeof(pe);

    if (Process32First(hSnap, &pe)) {
        do {
            /* Log every process for debugging the CE environment */
            MN1_LOG_INFO(L"  Process: PID=%d '%s'",
                         pe.th32ProcessID, pe.szExeFile);

            /* Check for MgrUSB.exe (case-insensitive) */
            /* Also check for common variants: mgrusb.exe, MGRUSB.EXE */
            WCHAR szLower[MAX_PATH];
            int i;
            for (i = 0; pe.szExeFile[i] && i < MAX_PATH - 1; i++)
                szLower[i] = (pe.szExeFile[i] >= L'A' && pe.szExeFile[i] <= L'Z')
                             ? pe.szExeFile[i] + 32 : pe.szExeFile[i];
            szLower[i] = 0;

            if (wcsstr(szLower, L"mgrusb") != NULL) {
                MN1_LOG_INFO(L"  >>> Found MgrUSB process: PID=%d", pe.th32ProcessID);
                bFound = TRUE;

                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc != NULL) {
                    if (TerminateProcess(hProc, 0)) {
                        MN1_LOG_INFO(L"  >>> MgrUSB.exe KILLED (PID=%d)", pe.th32ProcessID);
                        nKilled++;
                    } else {
                        MN1_LOG_ERROR(L"  >>> TerminateProcess failed: %d", GetLastError());
                    }
                    CloseHandle(hProc);
                } else {
                    MN1_LOG_ERROR(L"  >>> OpenProcess failed: %d", GetLastError());
                }
            }
        } while (Process32Next(hSnap, &pe));
    }

    CloseHandle(hSnap);

    if (bFound) {
        MN1_LOG_INFO(L"  Killed %d MgrUSB instance(s). Waiting 1s for USB release...", nKilled);
        Sleep(1000); /* Give the USB stack time to release resources */
    } else {
        MN1_LOG_INFO(L"  MgrUSB.exe not found (may already be killed or not running)");
    }

    return MN1_OK;
}

/* =========================================================================
 * PHASE 1a: TRY HCD STREAM DRIVER ACCESS
 *
 * Attempt to open the Host Controller Driver as a stream device.
 * WinCE HCDs typically register as stream drivers accessible via
 * CreateFile. Common names: "HCD1:", "UHC1:", "EHC1:", "OHC1:"
 * ========================================================================= */

static mn1_result_t try_hcd_stream(usb_device_state_t* pDev)
{
    /* HCD stream driver names to probe (in priority order) */
    static const WCHAR* hcdNames[] = {
        L"HCD1:",       /* Generic USB Host Controller Driver */
        L"EHC1:",       /* EHCI-specific driver */
        L"OHC1:",       /* OHCI-specific driver */
        L"UHC1:",       /* UHCI-specific driver */
        L"USB1:",       /* Some BSPs use this */
        NULL
    };

    USB_LOG_PHASE("PHASE 1a - Probe HCD Stream Drivers");

    for (int i = 0; hcdNames[i] != NULL; i++) {
        MN1_LOG_INFO(L"  Trying CreateFile('%s')...", hcdNames[i]);

        HANDLE hHCD = CreateFile(
            hcdNames[i],
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hHCD != INVALID_HANDLE_VALUE) {
            MN1_LOG_INFO(L"  SUCCESS: Opened '%s' handle=0x%p", hcdNames[i], hHCD);

            /* Verify the handle is functional by trying a simple IOCTL */
            DWORD dwFrameNum = 0;
            DWORD dwReturned = 0;
            BOOL bOk = DeviceIoControl(
                hHCD,
                IOCTL_HCD_GET_FRAME_NUMBER,
                NULL, 0,
                &dwFrameNum, sizeof(dwFrameNum),
                &dwReturned,
                NULL
            );

            if (bOk) {
                MN1_LOG_INFO(L"  HCD responds to IOCTL (frame=%d). Using HCD stream method.", dwFrameNum);
            } else {
                MN1_LOG_WARN(L"  HCD IOCTL failed (err=%d), but handle is open. Proceeding.", GetLastError());
            }

            pDev->hHCD = hHCD;
            pDev->accessMethod = USB_ACCESS_HCD_STREAM;
            return MN1_OK;
        } else {
            MN1_LOG_INFO(L"  '%s' not available (err=%d)", hcdNames[i], GetLastError());
        }
    }

    MN1_LOG_WARN(L"  No HCD stream driver found. Will try EHCI direct access.");
    return MN1_ERR_USB_INIT;
}

/* =========================================================================
 * PHASE 1b: DIRECT EHCI REGISTER ACCESS (MMIO FALLBACK)
 *
 * If no HCD stream driver is available, we map the EHCI controller's
 * physical registers into our virtual address space using VirtualAlloc
 * + VirtualCopy and drive the USB controller directly from user mode.
 *
 * This is the nuclear option but necessary on the stripped MediaNav.
 * ========================================================================= */

static mn1_result_t try_ehci_direct(usb_device_state_t* pDev)
{
    /* Physical addresses to probe for the EHCI controller */
    static const uint32_t physBases[] = {
        AU1320_USB_PHYS_BASE_0,     /* 0x14020000 - Primary */
        AU1320_USB_PHYS_BASE_1,     /* 0x14021000 - Alternate */
        AU1320_USB_PHYS_BASE_2,     /* 0x10100000 - Au1100 compat */
        0
    };

    USB_LOG_PHASE("PHASE 1b - Probe EHCI Direct Register Access");

    for (int i = 0; physBases[i] != 0; i++) {
        uint32_t physBase = physBases[i];
        void* pVirt = NULL;
        volatile uint32_t* pRegs = NULL;

        MN1_LOG_INFO(L"  Probing EHCI at physical 0x%08X...", physBase);

        /* Allocate virtual address space (4 pages = 16KB for EHCI) */
        pVirt = VirtualAlloc(0, 4 * 4096, MEM_RESERVE, PAGE_READWRITE);
        if (!pVirt) {
            MN1_LOG_ERROR(L"  VirtualAlloc failed: %d", GetLastError());
            continue;
        }

        /* Map physical memory to virtual space
         * WinCE 6.0 VirtualCopy: physical address must be >> 8 with PAGE_PHYSICAL */
        if (!VirtualCopy(
                pVirt,
                (void*)(physBase >> 8),
                4 * 4096,
                PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL))
        {
            MN1_LOG_INFO(L"  VirtualCopy failed for 0x%08X (err=%d)", physBase, GetLastError());
            VirtualFree(pVirt, 0, MEM_RELEASE);
            continue;
        }

        pRegs = (volatile uint32_t*)pVirt;

        /* Validate EHCI signature:
         * - CAPLENGTH at offset 0 should be 0x20 or similar small value
         * - HCIVERSION at offset 2 should be 0x0100 (EHCI 1.0) or 0x0110
         * - HCSPARAMS should have reasonable N_PORTS (1-15) */
        {
            uint8_t capLen = *((volatile uint8_t*)pRegs);
            uint16_t hciVer = *((volatile uint16_t*)((uint8_t*)pRegs + EHCI_CAP_HCIVERSION));
            uint32_t hcsParams = pRegs[EHCI_CAP_HCSPARAMS / 4];
            uint32_t nPorts = hcsParams & 0x0F;

            MN1_LOG_INFO(L"  CAPLENGTH=0x%02X, HCIVERSION=0x%04X, HCSPARAMS=0x%08X, N_PORTS=%d",
                         capLen, hciVer, hcsParams, nPorts);

            /* Sanity check: EHCI version should be 1.0 or 1.1 */
            if ((hciVer == 0x0100 || hciVer == 0x0110 || hciVer == 0x0095) &&
                capLen >= 0x08 && capLen <= 0x40 &&
                nPorts >= 1 && nPorts <= 15)
            {
                MN1_LOG_INFO(L"  EHCI controller FOUND at 0x%08X!", physBase);

                pDev->pEhciCap = pRegs;
                pDev->pEhciOp = (volatile uint32_t*)((uint8_t*)pRegs + capLen);
                pDev->dwEhciPhysBase = physBase;
                pDev->pEhciVirtBase = pVirt;
                pDev->dwCapLength = capLen;
                pDev->accessMethod = USB_ACCESS_EHCI_DIRECT;

                /* Log all operational registers */
                MN1_LOG_INFO(L"  USBCMD=0x%08X", pDev->pEhciOp[EHCI_OP_USBCMD / 4]);
                MN1_LOG_INFO(L"  USBSTS=0x%08X", pDev->pEhciOp[EHCI_OP_USBSTS / 4]);
                MN1_LOG_INFO(L"  PORTSC[0]=0x%08X", pDev->pEhciOp[EHCI_OP_PORTSC0 / 4]);

                return MN1_OK;
            } else {
                MN1_LOG_INFO(L"  Not a valid EHCI controller (bad signature)");

                /* Log raw register dump for reverse engineering */
                MN1_LOG_INFO(L"  RAW: [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X",
                             pRegs[0], pRegs[1], pRegs[2], pRegs[3]);
                MN1_LOG_INFO(L"  RAW: [4]=0x%08X [5]=0x%08X [6]=0x%08X [7]=0x%08X",
                             pRegs[4], pRegs[5], pRegs[6], pRegs[7]);
            }
        }

        VirtualFree(pVirt, 0, MEM_RELEASE);
    }

    MN1_LOG_ERROR(L"  No EHCI controller found at any known address.");
    return MN1_ERR_USB_INIT;
}

/* =========================================================================
 * EHCI PORT OPERATIONS
 * ========================================================================= */

static mn1_bool_t ehci_port_connected(usb_device_state_t* pDev)
{
    if (pDev->accessMethod != USB_ACCESS_EHCI_DIRECT)
        return FALSE;
    uint32_t portsc = pDev->pEhciOp[EHCI_OP_PORTSC0 / 4];
    return (portsc & EHCI_PORTSC_CONNECTED) ? TRUE : FALSE;
}

static mn1_result_t ehci_port_reset(usb_device_state_t* pDev)
{
    volatile uint32_t* pPortSC = &pDev->pEhciOp[EHCI_OP_PORTSC0 / 4];
    uint32_t portsc;
    int retry;

    MN1_LOG_INFO(L"  EHCI: Resetting port...");

    /* Read current PORTSC, preserve non-write-clear bits */
    portsc = *pPortSC;
    portsc &= ~EHCI_PORTSC_WRITE_CLEAR; /* Don't accidentally clear status bits */
    portsc &= ~EHCI_PORTSC_ENABLED;     /* Disable port during reset */

    /* Assert RESET */
    portsc |= EHCI_PORTSC_PORT_RESET;
    *pPortSC = portsc;
    MN1_LOG_INFO(L"  PORTSC after reset assert: 0x%08X", *pPortSC);

    /* USB spec: hold reset for at least 50ms */
    Sleep(60);

    /* De-assert RESET */
    portsc = *pPortSC;
    portsc &= ~EHCI_PORTSC_WRITE_CLEAR;
    portsc &= ~EHCI_PORTSC_PORT_RESET;
    *pPortSC = portsc;

    /* Wait for reset to complete and port to become enabled */
    for (retry = 0; retry < 20; retry++) {
        Sleep(10);
        portsc = *pPortSC;
        MN1_LOG_INFO(L"  PORTSC poll[%d]: 0x%08X", retry, portsc);

        if (!(portsc & EHCI_PORTSC_PORT_RESET) &&
            (portsc & EHCI_PORTSC_ENABLED))
        {
            MN1_LOG_INFO(L"  Port reset complete! Device is High-Speed=%s",
                         (portsc & EHCI_PORTSC_PORT_OWNER) ? L"No(companion)" : L"Yes");
            return MN1_OK;
        }
    }

    MN1_LOG_ERROR(L"  Port reset timed out (PORTSC=0x%08X)", *pPortSC);
    return MN1_ERR_USB_INIT;
}

/* =========================================================================
 * USB CONTROL TRANSFER ENGINE
 *
 * For HCD_STREAM method: uses DeviceIoControl with IOCTL_HCD_ISSUE_TRANSFER
 * For EHCI_DIRECT method: constructs QH/qTD structures in uncached memory
 *                         and submits to the async schedule (complex!)
 *
 * The HCD method is preferred. EHCI direct requires building EHCI data
 * structures (Queue Heads, Transfer Descriptors) which is extremely complex
 * to do correctly from user mode. We implement a simplified version that
 * works for the common case (control + bulk to a single device).
 * ========================================================================= */

/* Simplified EHCI QH and qTD for direct access mode */
#pragma pack(push, 32)  /* 32-byte alignment for EHCI */

typedef struct _EHCI_QTD {
    uint32_t    next_qtd;       /* Next qTD pointer (physical) */
    uint32_t    alt_next_qtd;   /* Alternate next (physical) */
    uint32_t    token;          /* Status + PID + control bits */
    uint32_t    buffer[5];      /* Buffer page pointers (physical) */
    /* Software fields (not read by hardware) */
    uint32_t    sw_buf_va;      /* Virtual address of data buffer */
    uint32_t    sw_next_va;     /* VA of next qTD */
    uint32_t    sw_pad[2];
} EHCI_QTD;

typedef struct _EHCI_QH {
    uint32_t    next_qh;        /* QH horizontal link pointer */
    uint32_t    characteristics;/* Endpoint characteristics */
    uint32_t    capabilities;   /* Endpoint capabilities */
    uint32_t    current_qtd;    /* Current qTD pointer */
    /* Overlay area (hardware copies qTD here) */
    uint32_t    overlay_next;
    uint32_t    overlay_alt;
    uint32_t    overlay_token;
    uint32_t    overlay_buffer[5];
    /* Software fields */
    uint32_t    sw_pad[4];
} EHCI_QH;

#pragma pack(pop)

/* EHCI qTD token fields */
#define QTD_TOKEN_STATUS_ACTIVE     (1 << 7)
#define QTD_TOKEN_STATUS_HALTED     (1 << 6)
#define QTD_TOKEN_STATUS_BUFERR     (1 << 5)
#define QTD_TOKEN_STATUS_BABBLE     (1 << 4)
#define QTD_TOKEN_STATUS_XACTERR    (1 << 3)
#define QTD_TOKEN_STATUS_MISSED     (1 << 2)
#define QTD_TOKEN_STATUS_SPLIT      (1 << 1)
#define QTD_TOKEN_STATUS_PING       (1 << 0)
#define QTD_TOKEN_PID_OUT           (0 << 8)
#define QTD_TOKEN_PID_IN            (1 << 8)
#define QTD_TOKEN_PID_SETUP         (2 << 8)
#define QTD_TOKEN_CERR(n)           (((n) & 3) << 10)
#define QTD_TOKEN_IOC               (1 << 15)
#define QTD_TOKEN_BYTES(n)          (((n) & 0x7FFF) << 16)
#define QTD_TOKEN_DT                (1 << 31)

#define QTD_NEXT_TERMINATE          1

/* QH characteristics fields */
#define QH_CHAR_ADDR(a)             ((a) & 0x7F)
#define QH_CHAR_EP(e)               (((e) & 0xF) << 8)
#define QH_CHAR_SPEED_HIGH          (2 << 12)
#define QH_CHAR_SPEED_FULL          (0 << 12)
#define QH_CHAR_DTC                 (1 << 14)    /* Data toggle from qTD */
#define QH_CHAR_HEAD                (1 << 15)    /* Head of reclamation list */
#define QH_CHAR_MPL(m)              (((m) & 0x7FF) << 16)
#define QH_CHAR_CONTROL_EP          (1 << 27)    /* For FS/LS control only */
#define QH_CHAR_NAK_RL(n)           (((n) & 0xF) << 28)

/* QH next pointer types */
#define QH_LINK_TYPE_QH             (1 << 1)
#define QH_LINK_TERMINATE           1

/* =========================================================================
 * CONTROL TRANSFER IMPLEMENTATION
 * ========================================================================= */

/* Via HCD stream driver */
static mn1_result_t ctrl_xfer_hcd(
    usb_device_state_t* pDev,
    const USB_SETUP_PACKET* pSetup,
    uint8_t*    pData,
    uint32_t    dwMaxLen,
    uint32_t*   pdwActual,
    uint32_t    dwTimeoutMs)
{
    /* Build the HCD_TRANSFER_REQUEST structure */
    uint8_t ioBuf[sizeof(HCD_TRANSFER_REQUEST) + 4096];
    HCD_TRANSFER_REQUEST* pReq = (HCD_TRANSFER_REQUEST*)ioBuf;
    DWORD dwReturned = 0;
    BOOL bOk;
    mn1_bool_t bDataIn = (pSetup->bmRequestType & 0x80) ? TRUE : FALSE;

    memset(pReq, 0, sizeof(HCD_TRANSFER_REQUEST));
    pReq->dwTransferType   = HCD_XFER_CONTROL;
    pReq->dwEndpoint       = 0;
    pReq->dwDirection      = bDataIn ? 1 : 0;
    pReq->dwBufferSize     = pSetup->wLength;
    pReq->dwTimeout        = dwTimeoutMs;
    pReq->dwDeviceAddress  = pDev->bDeviceAddress;
    pReq->dwMaxPacketSize  = pDev->bMaxPacketSize0 ? pDev->bMaxPacketSize0 : 64;
    memcpy(pReq->setupPacket, pSetup, 8);

    /* For OUT transfers, copy data after the request structure */
    if (!bDataIn && pData && pSetup->wLength > 0) {
        memcpy(ioBuf + sizeof(HCD_TRANSFER_REQUEST), pData, pSetup->wLength);
    }

    uint32_t dwInSize = sizeof(HCD_TRANSFER_REQUEST);
    if (!bDataIn && pSetup->wLength > 0)
        dwInSize += pSetup->wLength;

    /* Issue the IOCTL */
    bOk = DeviceIoControl(
        pDev->hHCD,
        IOCTL_HCD_ISSUE_TRANSFER,
        ioBuf,
        dwInSize,
        (bDataIn && pData) ? pData : NULL,
        bDataIn ? dwMaxLen : 0,
        &dwReturned,
        NULL
    );

    if (pdwActual)
        *pdwActual = dwReturned;

    if (!bOk) {
        MN1_LOG_ERROR(L"  HCD control transfer failed: err=%d", GetLastError());
        pDev->dwTransferErrors++;
        return MN1_ERR_USB_TRANSFER;
    }

    MN1_LOG_INFO(L"  HCD control OK: req=0x%02X val=0x%04X idx=0x%04X returned=%d",
                 pSetup->bRequest, pSetup->wValue, pSetup->wIndex, dwReturned);

    return MN1_OK;
}

/* Via direct EHCI register access (simplified polling mode) */
static mn1_result_t ctrl_xfer_ehci(
    usb_device_state_t* pDev,
    const USB_SETUP_PACKET* pSetup,
    uint8_t*    pData,
    uint32_t    dwMaxLen,
    uint32_t*   pdwActual,
    uint32_t    dwTimeoutMs)
{
    /*
     * EHCI direct control transfer is EXTREMELY complex to implement
     * correctly because we need to:
     * 1. Allocate physically contiguous, 32-byte aligned memory for QH/qTDs
     * 2. Fill in the SETUP qTD, DATA qTD(s), STATUS qTD
     * 3. Link them into a Queue Head
     * 4. Add the QH to the async schedule
     * 5. Enable the async schedule
     * 6. Poll for completion (or set up interrupt)
     * 7. Read back the results
     *
     * On WinCE 6.0, we get physically contiguous memory via
     * AllocPhysMem() or by using uncached VirtualAlloc regions.
     *
     * For the MediaNav, we implement a simplified polling approach:
     * - Single QH + single chain of qTDs
     * - Polling mode (no interrupts - we're user mode)
     * - One transfer at a time (no concurrent transfers)
     */

    /* AllocPhysMem for DMA-able memory */
    /* Note: WinCE 6.0 exposes AllocPhysMem in coredll.dll */
    typedef LPVOID (WINAPI *PFN_AllocPhysMem)(DWORD, DWORD, DWORD, DWORD, PULONG);
    typedef BOOL (WINAPI *PFN_FreePhysMem)(LPVOID);

    static PFN_AllocPhysMem pfnAlloc = NULL;
    static PFN_FreePhysMem pfnFree = NULL;

    if (!pfnAlloc) {
        HMODULE hCore = GetModuleHandle(L"coredll.dll");
        if (hCore) {
            pfnAlloc = (PFN_AllocPhysMem)GetProcAddress(hCore, L"AllocPhysMem");
            pfnFree = (PFN_FreePhysMem)GetProcAddress(hCore, L"FreePhysMem");
        }
    }

    if (!pfnAlloc || !pfnFree) {
        MN1_LOG_ERROR(L"  AllocPhysMem not available - cannot do EHCI direct transfers");
        return MN1_ERR_USB_TRANSFER;
    }

    mn1_bool_t bDataIn = (pSetup->bmRequestType & 0x80) ? TRUE : FALSE;
    uint32_t dwDataLen = pSetup->wLength;

    /* Allocate physically contiguous memory for QH + qTDs + data buffer
     * Layout: [QH 64B][SETUP_qTD 32B][DATA_qTD 32B][STATUS_qTD 32B][SETUP_DATA 8B][XFER_DATA 4096B]
     * Total: ~4256 bytes, allocate 8192 for alignment safety */
    ULONG physAddr = 0;
    uint8_t* pDmaMem = (uint8_t*)pfnAlloc(8192, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &physAddr);
    if (!pDmaMem || physAddr == 0) {
        MN1_LOG_ERROR(L"  AllocPhysMem failed for EHCI transfer memory");
        return MN1_ERR_OUT_OF_MEMORY;
    }

    memset(pDmaMem, 0, 8192);

    /* Compute physical addresses for each structure */
    EHCI_QH*  pQH        = (EHCI_QH*)(pDmaMem);
    EHCI_QTD* pSetupTD   = (EHCI_QTD*)(pDmaMem + 64);
    EHCI_QTD* pDataTD    = (EHCI_QTD*)(pDmaMem + 96);
    EHCI_QTD* pStatusTD  = (EHCI_QTD*)(pDmaMem + 128);
    uint8_t*  pSetupBuf  = pDmaMem + 160;
    uint8_t*  pDataBuf   = pDmaMem + 192;

    uint32_t paQH       = physAddr;
    uint32_t paSetupTD  = physAddr + 64;
    uint32_t paDataTD   = physAddr + 96;
    uint32_t paStatusTD = physAddr + 128;
    uint32_t paSetupBuf = physAddr + 160;
    uint32_t paDataBuf  = physAddr + 192;

    /* Copy setup packet */
    memcpy(pSetupBuf, pSetup, 8);

    /* Copy OUT data if applicable */
    if (!bDataIn && pData && dwDataLen > 0)
        memcpy(pDataBuf, pData, dwDataLen > 4096 ? 4096 : dwDataLen);

    /* --- Build SETUP qTD --- */
    pSetupTD->next_qtd     = (dwDataLen > 0) ? paDataTD : paStatusTD;
    pSetupTD->alt_next_qtd = QTD_NEXT_TERMINATE;
    pSetupTD->token        = QTD_TOKEN_STATUS_ACTIVE | QTD_TOKEN_PID_SETUP |
                             QTD_TOKEN_CERR(3) | QTD_TOKEN_BYTES(8);
    pSetupTD->buffer[0]    = paSetupBuf;

    /* --- Build DATA qTD (if needed) --- */
    if (dwDataLen > 0) {
        pDataTD->next_qtd     = paStatusTD;
        pDataTD->alt_next_qtd = QTD_NEXT_TERMINATE;
        pDataTD->token        = QTD_TOKEN_STATUS_ACTIVE | QTD_TOKEN_DT |
                                QTD_TOKEN_CERR(3) | QTD_TOKEN_BYTES(dwDataLen) |
                                (bDataIn ? QTD_TOKEN_PID_IN : QTD_TOKEN_PID_OUT);
        pDataTD->buffer[0]    = paDataBuf;
    }

    /* --- Build STATUS qTD --- */
    pStatusTD->next_qtd     = QTD_NEXT_TERMINATE;
    pStatusTD->alt_next_qtd = QTD_NEXT_TERMINATE;
    pStatusTD->token        = QTD_TOKEN_STATUS_ACTIVE | QTD_TOKEN_IOC |
                              QTD_TOKEN_DT | QTD_TOKEN_CERR(3) |
                              QTD_TOKEN_BYTES(0) |
                              (bDataIn ? QTD_TOKEN_PID_OUT : QTD_TOKEN_PID_IN);

    /* --- Build QH --- */
    pQH->next_qh = paQH | QH_LINK_TYPE_QH; /* Self-loop (single QH in schedule) */
    pQH->characteristics = QH_CHAR_ADDR(pDev->bDeviceAddress) |
                           QH_CHAR_EP(0) |
                           QH_CHAR_SPEED_HIGH |
                           QH_CHAR_DTC |
                           QH_CHAR_HEAD |
                           QH_CHAR_MPL(pDev->bMaxPacketSize0 ? pDev->bMaxPacketSize0 : 64) |
                           QH_CHAR_NAK_RL(4);
    pQH->capabilities = (1 << 30); /* Mult=1 for HS */
    pQH->current_qtd = 0;
    pQH->overlay_next = paSetupTD;
    pQH->overlay_alt  = QTD_NEXT_TERMINATE;
    pQH->overlay_token = 0; /* Clear overlay so HW fetches qTD */

    /* --- Submit to async schedule --- */
    /* Write async list address */
    pDev->pEhciOp[EHCI_OP_ASYNCLISTADDR / 4] = paQH;

    /* Enable async schedule + run */
    uint32_t cmd = pDev->pEhciOp[EHCI_OP_USBCMD / 4];
    cmd |= EHCI_CMD_ASYNC_EN | EHCI_CMD_RUN;
    pDev->pEhciOp[EHCI_OP_USBCMD / 4] = cmd;

    /* --- Poll for completion --- */
    uint32_t dwStart = GetTickCount();
    mn1_result_t result = MN1_ERR_TIMEOUT;
    uint32_t dwActual = 0;

    while ((GetTickCount() - dwStart) < dwTimeoutMs) {
        /* Check if status qTD completed (active bit cleared) */
        uint32_t statusToken = pStatusTD->token;
        uint32_t dataToken = (dwDataLen > 0) ? pDataTD->token : 0;
        uint32_t setupToken = pSetupTD->token;

        if (!(statusToken & QTD_TOKEN_STATUS_ACTIVE)) {
            /* Transfer complete! Check for errors */
            if (statusToken & (QTD_TOKEN_STATUS_HALTED | QTD_TOKEN_STATUS_BUFERR |
                               QTD_TOKEN_STATUS_BABBLE | QTD_TOKEN_STATUS_XACTERR))
            {
                MN1_LOG_ERROR(L"  EHCI ctrl xfer error: token=0x%08X", statusToken);
                result = MN1_ERR_USB_TRANSFER;
            } else {
                /* Success! Calculate bytes transferred */
                if (dwDataLen > 0) {
                    uint32_t bytesLeft = (dataToken >> 16) & 0x7FFF;
                    dwActual = dwDataLen - bytesLeft;
                }
                result = MN1_OK;
            }
            break;
        }

        /* Check for setup/data errors */
        if ((setupToken & QTD_TOKEN_STATUS_HALTED) ||
            (dwDataLen > 0 && (dataToken & QTD_TOKEN_STATUS_HALTED)))
        {
            MN1_LOG_ERROR(L"  EHCI ctrl xfer HALTED: setup=0x%08X data=0x%08X",
                         setupToken, dataToken);
            result = MN1_ERR_USB_TRANSFER;
            break;
        }

        Sleep(1); /* Yield while polling */
    }

    /* Disable async schedule after transfer */
    cmd = pDev->pEhciOp[EHCI_OP_USBCMD / 4];
    cmd &= ~EHCI_CMD_ASYNC_EN;
    pDev->pEhciOp[EHCI_OP_USBCMD / 4] = cmd;

    /* Copy IN data back to caller */
    if (result == MN1_OK && bDataIn && pData && dwActual > 0) {
        memcpy(pData, pDataBuf, dwActual > dwMaxLen ? dwMaxLen : dwActual);
    }

    if (pdwActual)
        *pdwActual = dwActual;

    MN1_LOG_INFO(L"  EHCI ctrl xfer: result=%d actual=%d (req=0x%02X val=0x%04X)",
                 result, dwActual, pSetup->bRequest, pSetup->wValue);

    pfnFree(pDmaMem);
    return result;
}

/* =========================================================================
 * PUBLIC: CONTROL TRANSFER DISPATCHER
 * ========================================================================= */

mn1_result_t usb_handler_control_transfer(
    usb_device_state_t* pDev,
    const USB_SETUP_PACKET* pSetup,
    uint8_t*            pData,
    uint32_t            dwMaxLen,
    uint32_t*           pdwActualLen,
    uint32_t            dwTimeoutMs)
{
    USB_LOG_HEX("SETUP", pSetup, 8);

    switch (pDev->accessMethod) {
        case USB_ACCESS_HCD_STREAM:
            return ctrl_xfer_hcd(pDev, pSetup, pData, dwMaxLen, pdwActualLen, dwTimeoutMs);
        case USB_ACCESS_EHCI_DIRECT:
            return ctrl_xfer_ehci(pDev, pSetup, pData, dwMaxLen, pdwActualLen, dwTimeoutMs);
        default:
            return MN1_ERR_USB_INIT;
    }
}

/* =========================================================================
 * BULK TRANSFER IMPLEMENTATION
 * ========================================================================= */

static mn1_result_t bulk_xfer_hcd(
    usb_device_state_t* pDev,
    uint8_t     bEndpoint,
    uint8_t*    pBuffer,
    uint32_t    dwLen,
    uint32_t*   pdwActual,
    uint32_t    dwTimeoutMs,
    mn1_bool_t  bIn)
{
    uint8_t ioBuf[sizeof(HCD_TRANSFER_REQUEST) + MN1_USB_RX_BUFFER_SIZE];
    HCD_TRANSFER_REQUEST* pReq = (HCD_TRANSFER_REQUEST*)ioBuf;
    DWORD dwReturned = 0;
    BOOL bOk;

    memset(pReq, 0, sizeof(HCD_TRANSFER_REQUEST));
    pReq->dwTransferType   = HCD_XFER_BULK;
    pReq->dwEndpoint       = bEndpoint;
    pReq->dwDirection      = bIn ? 1 : 0;
    pReq->dwBufferSize     = dwLen;
    pReq->dwTimeout        = dwTimeoutMs;
    pReq->dwDeviceAddress  = pDev->bDeviceAddress;
    pReq->dwMaxPacketSize  = bIn ? pDev->epBulkIn.wMaxPacketSize : pDev->epBulkOut.wMaxPacketSize;

    if (!bIn && pBuffer && dwLen > 0) {
        uint32_t copyLen = dwLen;
        if (copyLen > MN1_USB_RX_BUFFER_SIZE)
            copyLen = MN1_USB_RX_BUFFER_SIZE;
        memcpy(ioBuf + sizeof(HCD_TRANSFER_REQUEST), pBuffer, copyLen);
    }

    bOk = DeviceIoControl(
        pDev->hHCD,
        IOCTL_HCD_ISSUE_TRANSFER,
        ioBuf,
        bIn ? sizeof(HCD_TRANSFER_REQUEST) : (sizeof(HCD_TRANSFER_REQUEST) + dwLen),
        bIn ? pBuffer : NULL,
        bIn ? dwLen : 0,
        &dwReturned,
        NULL
    );

    if (pdwActual)
        *pdwActual = dwReturned;

    if (!bOk) {
        DWORD err = GetLastError();
        if (err == ERROR_TIMEOUT || err == ERROR_SEM_TIMEOUT)
            return MN1_ERR_TIMEOUT;
        pDev->dwTransferErrors++;
        return MN1_ERR_USB_TRANSFER;
    }

    if (bIn)
        pDev->dwTotalBytesIn += dwReturned;
    else
        pDev->dwTotalBytesOut += dwLen;

    return MN1_OK;
}

mn1_result_t usb_handler_bulk_read(
    usb_device_state_t* pDev,
    uint8_t*            pBuffer,
    uint32_t            dwMaxBytes,
    uint32_t*           pdwActual,
    uint32_t            dwTimeoutMs)
{
    if (!pDev->bConfigured || !pDev->epBulkIn.bAddress)
        return MN1_ERR_USB_NO_DEVICE;

    return bulk_xfer_hcd(pDev, pDev->epBulkIn.bAddress,
                          pBuffer, dwMaxBytes, pdwActual, dwTimeoutMs, TRUE);
}

mn1_result_t usb_handler_bulk_write(
    usb_device_state_t* pDev,
    const uint8_t*      pBuffer,
    uint32_t            dwBytes,
    uint32_t*           pdwActual,
    uint32_t            dwTimeoutMs)
{
    if (!pDev->bConfigured || !pDev->epBulkOut.bAddress)
        return MN1_ERR_USB_NO_DEVICE;

    return bulk_xfer_hcd(pDev, pDev->epBulkOut.bAddress,
                          (uint8_t*)pBuffer, dwBytes, pdwActual, dwTimeoutMs, FALSE);
}

/* =========================================================================
 * PHASE 2: USB DEVICE ENUMERATION
 * ========================================================================= */

mn1_result_t usb_handler_enumerate(usb_device_state_t* pDev, uint32_t dwTimeoutMs)
{
    USB_DEVICE_DESC devDesc;
    uint8_t configBuf[256]; /* Config descriptor + interfaces + endpoints */
    uint32_t dwActual = 0;
    mn1_result_t result;
    USB_SETUP_PACKET setup;

    USB_LOG_PHASE("PHASE 2 - USB Device Enumeration");

    /* --- Step 2.1: Wait for device connection --- */
    if (pDev->accessMethod == USB_ACCESS_EHCI_DIRECT) {
        uint32_t dwStart = GetTickCount();
        MN1_LOG_INFO(L"  Waiting for device connection (EHCI direct)...");

        while ((GetTickCount() - dwStart) < dwTimeoutMs) {
            if (ehci_port_connected(pDev)) {
                uint32_t portsc = pDev->pEhciOp[EHCI_OP_PORTSC0 / 4];
                MN1_LOG_INFO(L"  DEVICE CONNECTED! PORTSC=0x%08X", portsc);
                break;
            }
            Sleep(100);
        }

        if (!ehci_port_connected(pDev)) {
            MN1_LOG_ERROR(L"  No device connected within %dms", dwTimeoutMs);
            return MN1_ERR_USB_NO_DEVICE;
        }

        /* --- Step 2.2: Port Reset --- */
        result = ehci_port_reset(pDev);
        if (result != MN1_OK)
            return result;
    } else {
        /* HCD stream handles connection detection internally */
        MN1_LOG_INFO(L"  Using HCD stream - port management by driver");
    }

    pDev->bDevicePresent = TRUE;
    pDev->bDeviceAddress = 0; /* Default address after reset */
    pDev->bMaxPacketSize0 = 8; /* Minimum, updated after first descriptor read */

    /* --- Step 2.3: GET_DESCRIPTOR (Device, first 8 bytes) ---
     * Read only 8 bytes first to learn bMaxPacketSize0 */
    MN1_LOG_INFO(L"  GET_DESCRIPTOR (Device, 8 bytes, addr=0)...");
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x80; /* IN, Standard, Device */
    setup.bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup.wValue        = (USB_DESC_DEVICE << 8) | 0;
    setup.wIndex        = 0;
    setup.wLength       = 8;

    result = usb_handler_control_transfer(pDev, &setup,
                (uint8_t*)&devDesc, 8, &dwActual, 5000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  Failed to read device descriptor (8B): %d", result);
        return result;
    }

    pDev->bMaxPacketSize0 = devDesc.bMaxPacketSize0;
    MN1_LOG_INFO(L"  bMaxPacketSize0 = %d", pDev->bMaxPacketSize0);

    /* --- Step 2.4: SET_ADDRESS --- */
    pDev->bDeviceAddress = 1; /* Assign address 1 */
    MN1_LOG_INFO(L"  SET_ADDRESS(%d)...", pDev->bDeviceAddress);
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x00; /* OUT, Standard, Device */
    setup.bRequest      = USB_REQ_SET_ADDRESS;
    setup.wValue        = pDev->bDeviceAddress;
    setup.wIndex        = 0;
    setup.wLength       = 0;

    /* Temporarily set address to 0 for this transfer, then update */
    uint8_t savedAddr = pDev->bDeviceAddress;
    pDev->bDeviceAddress = 0;
    result = usb_handler_control_transfer(pDev, &setup, NULL, 0, NULL, 2000);
    pDev->bDeviceAddress = savedAddr;

    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  SET_ADDRESS failed: %d", result);
        return result;
    }

    Sleep(10); /* SetAddress recovery time (2ms min per spec) */
    MN1_LOG_INFO(L"  Device address set to %d", pDev->bDeviceAddress);

    /* --- Step 2.5: GET_DESCRIPTOR (Device, full 18 bytes) --- */
    MN1_LOG_INFO(L"  GET_DESCRIPTOR (Device, full, addr=%d)...", pDev->bDeviceAddress);
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x80;
    setup.bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup.wValue        = (USB_DESC_DEVICE << 8) | 0;
    setup.wIndex        = 0;
    setup.wLength       = sizeof(USB_DEVICE_DESC);

    result = usb_handler_control_transfer(pDev, &setup,
                (uint8_t*)&devDesc, sizeof(USB_DEVICE_DESC), &dwActual, 5000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  Failed to read full device descriptor: %d", result);
        return result;
    }

    pDev->wVendorId  = devDesc.idVendor;
    pDev->wProductId = devDesc.idProduct;

    MN1_LOG_INFO(L"  ===== DEVICE DESCRIPTOR =====");
    MN1_LOG_INFO(L"  bcdUSB:          0x%04X", devDesc.bcdUSB);
    MN1_LOG_INFO(L"  bDeviceClass:    0x%02X", devDesc.bDeviceClass);
    MN1_LOG_INFO(L"  bDeviceSubClass: 0x%02X", devDesc.bDeviceSubClass);
    MN1_LOG_INFO(L"  bDeviceProtocol: 0x%02X", devDesc.bDeviceProtocol);
    MN1_LOG_INFO(L"  bMaxPacketSize0: %d", devDesc.bMaxPacketSize0);
    MN1_LOG_INFO(L"  idVendor:        0x%04X", devDesc.idVendor);
    MN1_LOG_INFO(L"  idProduct:       0x%04X", devDesc.idProduct);
    MN1_LOG_INFO(L"  bcdDevice:       0x%04X", devDesc.bcdDevice);
    MN1_LOG_INFO(L"  bNumConfigs:     %d", devDesc.bNumConfigurations);
    MN1_LOG_INFO(L"  =============================");

    /* Check if already in AOA mode */
    if (devDesc.idVendor == GOOGLE_VID &&
        (devDesc.idProduct >= AOA_PID_ACCESSORY && devDesc.idProduct <= AOA_PID_ACC_AUDIO_ADB))
    {
        MN1_LOG_INFO(L"  >>> Device is ALREADY in AOA mode! (PID=0x%04X)", devDesc.idProduct);
        pDev->bAOAMode = TRUE;
    }

    /* --- Step 2.6: GET_DESCRIPTOR (Configuration) --- */
    MN1_LOG_INFO(L"  GET_DESCRIPTOR (Configuration)...");
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x80;
    setup.bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup.wValue        = (USB_DESC_CONFIGURATION << 8) | 0;
    setup.wIndex        = 0;
    setup.wLength       = sizeof(configBuf);

    result = usb_handler_control_transfer(pDev, &setup,
                configBuf, sizeof(configBuf), &dwActual, 5000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  Failed to read config descriptor: %d", result);
        return result;
    }

    USB_CONFIG_DESC* pConfig = (USB_CONFIG_DESC*)configBuf;
    MN1_LOG_INFO(L"  Config: wTotalLength=%d, bNumInterfaces=%d, bConfigValue=%d",
                 pConfig->wTotalLength, pConfig->bNumInterfaces, pConfig->bConfigurationValue);

    /* --- Step 2.7: Parse endpoints from configuration descriptor --- */
    {
        uint32_t offset = pConfig->bLength;
        while (offset + 2 <= dwActual) {
            uint8_t len  = configBuf[offset];
            uint8_t type = configBuf[offset + 1];

            if (len < 2) break;

            if (type == USB_DESC_INTERFACE) {
                USB_INTERFACE_DESC* pIface = (USB_INTERFACE_DESC*)&configBuf[offset];
                MN1_LOG_INFO(L"  Interface %d: class=0x%02X sub=0x%02X proto=0x%02X eps=%d",
                             pIface->bInterfaceNumber, pIface->bInterfaceClass,
                             pIface->bInterfaceSubClass, pIface->bInterfaceProtocol,
                             pIface->bNumEndpoints);
            }
            else if (type == USB_DESC_ENDPOINT) {
                USB_ENDPOINT_DESC* pEp = (USB_ENDPOINT_DESC*)&configBuf[offset];
                uint8_t epType = pEp->bmAttributes & USB_EP_TYPE_MASK;
                mn1_bool_t bEpIn = (pEp->bEndpointAddress & USB_EP_DIR_IN) ? TRUE : FALSE;

                MN1_LOG_INFO(L"  Endpoint 0x%02X: type=%d dir=%s maxpkt=%d",
                             pEp->bEndpointAddress, epType,
                             bEpIn ? L"IN" : L"OUT", pEp->wMaxPacketSize);

                if (epType == USB_EP_TYPE_BULK) {
                    if (bEpIn && pDev->epBulkIn.bAddress == 0) {
                        pDev->epBulkIn.bAddress       = pEp->bEndpointAddress;
                        pDev->epBulkIn.bType           = USB_EP_TYPE_BULK;
                        pDev->epBulkIn.wMaxPacketSize  = pEp->wMaxPacketSize;
                        MN1_LOG_INFO(L"  >>> Bulk IN endpoint discovered: 0x%02X (maxpkt=%d)",
                                     pEp->bEndpointAddress, pEp->wMaxPacketSize);
                    }
                    else if (!bEpIn && pDev->epBulkOut.bAddress == 0) {
                        pDev->epBulkOut.bAddress       = pEp->bEndpointAddress;
                        pDev->epBulkOut.bType           = USB_EP_TYPE_BULK;
                        pDev->epBulkOut.wMaxPacketSize  = pEp->wMaxPacketSize;
                        MN1_LOG_INFO(L"  >>> Bulk OUT endpoint discovered: 0x%02X (maxpkt=%d)",
                                     pEp->bEndpointAddress, pEp->wMaxPacketSize);
                    }
                }
            }

            offset += len;
        }
    }

    /* --- Step 2.8: SET_CONFIGURATION --- */
    MN1_LOG_INFO(L"  SET_CONFIGURATION(%d)...", pConfig->bConfigurationValue);
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x00;
    setup.bRequest      = USB_REQ_SET_CONFIG;
    setup.wValue        = pConfig->bConfigurationValue;
    setup.wIndex        = 0;
    setup.wLength       = 0;

    result = usb_handler_control_transfer(pDev, &setup, NULL, 0, NULL, 2000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  SET_CONFIGURATION failed: %d", result);
        return result;
    }

    pDev->bConfigured = TRUE;
    MN1_LOG_INFO(L"  Device configured successfully!");
    MN1_LOG_INFO(L"  VID:PID = %04X:%04X, BulkIn=0x%02X, BulkOut=0x%02X",
                 pDev->wVendorId, pDev->wProductId,
                 pDev->epBulkIn.bAddress, pDev->epBulkOut.bAddress);

    return MN1_OK;
}

/* =========================================================================
 * PHASE 3: AOA v2.0 HANDSHAKE
 * ========================================================================= */

/* AOA identity strings (sent to Android device) */
static const char* s_aoaStrings[6] = {
    MN1_AOA_MANUFACTURER,   /* Index 0 */
    MN1_AOA_MODEL,          /* Index 1 */
    MN1_AOA_DESCRIPTION,    /* Index 2 */
    MN1_AOA_VERSION,        /* Index 3 */
    MN1_AOA_URI,            /* Index 4 */
    MN1_AOA_SERIAL          /* Index 5 */
};

mn1_result_t usb_handler_aoa_handshake(usb_device_state_t* pDev)
{
    USB_SETUP_PACKET setup;
    mn1_result_t result;
    uint8_t protoData[2];
    uint32_t dwActual;

    USB_LOG_PHASE("PHASE 3 - AOA v2.0 Handshake");

    /* --- Step 3.0: Check if already AOA --- */
    if (pDev->bAOAMode) {
        MN1_LOG_INFO(L"  Device already in AOA mode. Skipping handshake.");
        return MN1_OK;
    }

    /* --- Step 3.1: GET_PROTOCOL (vendor request 51) --- */
    MN1_LOG_INFO(L"  AOA Step 1: GET_PROTOCOL...");
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0xC0; /* Device-to-Host, Vendor, Device */
    setup.bRequest      = AOA_REQ_GET_PROTOCOL;
    setup.wValue        = 0;
    setup.wIndex        = 0;
    setup.wLength       = 2;

    result = usb_handler_control_transfer(pDev, &setup, protoData, 2, &dwActual, 5000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  GET_PROTOCOL failed. Device may not support AOA.");
        return MN1_ERR_USB_AOA_UNSUPPORTED;
    }

    pDev->wAOAProtocolVer = (uint16_t)(protoData[0] | (protoData[1] << 8));
    MN1_LOG_INFO(L"  AOA Protocol Version: %d", pDev->wAOAProtocolVer);

    if (pDev->wAOAProtocolVer == 0) {
        MN1_LOG_ERROR(L"  AOA not supported (version 0)");
        return MN1_ERR_USB_AOA_UNSUPPORTED;
    }
    if (pDev->wAOAProtocolVer >= 2) {
        MN1_LOG_INFO(L"  AOA v2.0 features available (audio routing, HID)");
    }

    /* --- Step 3.2: SEND_STRING (vendor request 52, x6) --- */
    MN1_LOG_INFO(L"  AOA Step 2: SEND_STRING (6 identity strings)...");
    for (int i = 0; i < 6; i++) {
        const char* str = s_aoaStrings[i];
        uint16_t len = 0;
        while (str[len] != '\0') len++;
        len++; /* Include null terminator */

        MN1_LOG_INFO(L"  AOA String[%d] = \"%hs\" (%d bytes)", i, str, len);

        memset(&setup, 0, sizeof(setup));
        setup.bmRequestType = 0x40; /* Host-to-Device, Vendor, Device */
        setup.bRequest      = AOA_REQ_SEND_STRING;
        setup.wValue        = 0;
        setup.wIndex        = (uint16_t)i;
        setup.wLength       = len;

        result = usb_handler_control_transfer(pDev, &setup,
                    (uint8_t*)str, len, NULL, 2000);
        if (result != MN1_OK) {
            MN1_LOG_ERROR(L"  SEND_STRING[%d] failed: %d", i, result);
            return MN1_ERR_USB_AOA_HANDSHAKE;
        }

        Sleep(15); /* Inter-string delay for Android processing */
    }

    /* --- Step 3.3: AOA v2.0 Set Audio Mode (optional) ---
     * If protocol >= 2, we can configure audio routing.
     * Mode 0 = no audio, Mode 1 = route audio to accessory over USB */
    if (pDev->wAOAProtocolVer >= 2) {
        MN1_LOG_INFO(L"  AOA Step 2.5: SET_AUDIO_MODE (mode=0, no audio)...");
        memset(&setup, 0, sizeof(setup));
        setup.bmRequestType = 0x40;
        setup.bRequest      = AOA_REQ_SET_AUDIO_MODE;
        setup.wValue        = 0; /* Mode 0 = no audio (save CPU cycles) */
        setup.wIndex        = 0;
        setup.wLength       = 0;

        result = usb_handler_control_transfer(pDev, &setup, NULL, 0, NULL, 2000);
        if (result != MN1_OK) {
            MN1_LOG_WARN(L"  SET_AUDIO_MODE failed (non-fatal): %d", result);
        }
    }

    /* --- Step 3.4: START (vendor request 53) --- */
    MN1_LOG_INFO(L"  AOA Step 3: START (entering accessory mode)...");
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x40;
    setup.bRequest      = AOA_REQ_START;
    setup.wValue        = 0;
    setup.wIndex        = 0;
    setup.wLength       = 0;

    result = usb_handler_control_transfer(pDev, &setup, NULL, 0, NULL, 2000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  AOA START failed: %d", result);
        return MN1_ERR_USB_AOA_HANDSHAKE;
    }

    /* --- Step 3.5: Wait for device re-enumeration ---
     * After START, the Android device disconnects and re-enumerates
     * with VID:PID 18D1:2D00 (or 2D01 with ADB) */
    MN1_LOG_INFO(L"  AOA Step 4: Waiting for device re-enumeration...");
    MN1_LOG_INFO(L"  (Device will disconnect and reconnect as AOA device)");

    /* Clear current device state */
    pDev->bDevicePresent = FALSE;
    pDev->bConfigured    = FALSE;
    pDev->bAOAMode       = FALSE;
    pDev->epBulkIn.bAddress  = 0;
    pDev->epBulkOut.bAddress = 0;

    Sleep(2000); /* Wait for USB disconnect/reconnect cycle */

    /* Re-enumerate the device (now should appear as AOA) */
    result = usb_handler_enumerate(pDev, 15000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"  AOA re-enumeration failed: %d", result);
        return MN1_ERR_USB_AOA_HANDSHAKE;
    }

    /* Verify it's now an AOA device */
    if (pDev->wVendorId == GOOGLE_VID &&
        (pDev->wProductId >= AOA_PID_ACCESSORY && pDev->wProductId <= AOA_PID_ACC_AUDIO_ADB))
    {
        pDev->bAOAMode = TRUE;
        MN1_LOG_INFO(L"  ==========================================");
        MN1_LOG_INFO(L"  AOA HANDSHAKE COMPLETE!");
        MN1_LOG_INFO(L"  VID:PID = %04X:%04X", pDev->wVendorId, pDev->wProductId);
        MN1_LOG_INFO(L"  Bulk IN  = 0x%02X (maxpkt=%d)",
                     pDev->epBulkIn.bAddress, pDev->epBulkIn.wMaxPacketSize);
        MN1_LOG_INFO(L"  Bulk OUT = 0x%02X (maxpkt=%d)",
                     pDev->epBulkOut.bAddress, pDev->epBulkOut.wMaxPacketSize);
        MN1_LOG_INFO(L"  ==========================================");
        return MN1_OK;
    } else {
        MN1_LOG_ERROR(L"  Device re-enumerated but VID:PID mismatch: %04X:%04X",
                     pDev->wVendorId, pDev->wProductId);
        return MN1_ERR_USB_AOA_HANDSHAKE;
    }
}

/* =========================================================================
 * PHASE 4: WORKER THREADS (High-Priority Bulk Endpoint Management)
 *
 * Two dedicated threads compensate for the weak MIPS CPU:
 *
 * 1. Video IN Thread (THREAD_PRIORITY_HIGHEST):
 *    - Continuously reads bulk IN data (H.264/video frames)
 *    - Double-buffers: while one buffer is being consumed by the
 *      decoder, the other receives new data
 *    - Signals main thread when a new frame buffer is ready
 *
 * 2. Touch OUT Thread (THREAD_PRIORITY_ABOVE_NORMAL):
 *    - Waits for touch events queued by the main thread
 *    - Sends them via bulk OUT as quickly as possible
 *    - Lower priority than video because touch latency is
 *      less critical than maintaining frame rate
 * ========================================================================= */

static DWORD WINAPI video_in_thread_proc(LPVOID lpParam)
{
    usb_device_state_t* pDev = (usb_device_state_t*)lpParam;
    uint32_t dwActual;
    mn1_result_t result;
    int bufIdx = 0;

    MN1_LOG_INFO(L"[VideoInThread] Started (priority=HIGHEST)");

    while (pDev->bThreadsRunning) {
        /* Read into the inactive buffer */
        int writeBuf = (pDev->nActiveVideoBuf == 0) ? 1 : 0;

        result = usb_handler_bulk_read(
            pDev,
            pDev->pVideoRxBuf[writeBuf],
            MN1_USB_RX_BUFFER_SIZE,
            &dwActual,
            1000  /* 1 second timeout (will retry) */
        );

        if (result == MN1_OK && dwActual > 0) {
            pDev->dwVideoRxLen[writeBuf] = dwActual;

            /* Swap buffers (atomic via critical section) */
            EnterCriticalSection(&pDev->csVideoSwap);
            pDev->nActiveVideoBuf = writeBuf;
            LeaveCriticalSection(&pDev->csVideoSwap);

            /* Signal that a new frame is available */
            SetEvent(pDev->hVideoInEvent);

        } else if (result == MN1_ERR_TIMEOUT) {
            /* Normal timeout - no data available, just retry */
            continue;
        } else {
            /* Error */
            MN1_LOG_WARN(L"[VideoInThread] Bulk read error: %d (retrying...)", result);
            pDev->dwTransferErrors++;

            /* Back off on repeated errors */
            Sleep(50);
            if (pDev->dwTransferErrors > 100) {
                MN1_LOG_ERROR(L"[VideoInThread] Too many errors. Stopping.");
                pDev->bThreadsRunning = FALSE;
                break;
            }
        }
    }

    MN1_LOG_INFO(L"[VideoInThread] Exiting");
    return 0;
}

static DWORD WINAPI touch_out_thread_proc(LPVOID lpParam)
{
    usb_device_state_t* pDev = (usb_device_state_t*)lpParam;
    uint32_t dwActual;
    mn1_result_t result;

    MN1_LOG_INFO(L"[TouchOutThread] Started (priority=ABOVE_NORMAL)");

    while (pDev->bThreadsRunning) {
        /* Wait for a touch event to be queued */
        DWORD waitResult = WaitForSingleObject(pDev->hTouchOutEvent, 100);

        if (waitResult == WAIT_OBJECT_0 && pDev->bTouchPending) {
            mn1_touch_event_t event = pDev->pendingTouch;
            pDev->bTouchPending = FALSE;

            result = usb_handler_bulk_write(
                pDev,
                (const uint8_t*)&event,
                sizeof(mn1_touch_event_t),
                &dwActual,
                200  /* Touch needs low latency */
            );

            if (result != MN1_OK) {
                MN1_LOG_WARN(L"[TouchOutThread] Send failed: %d", result);
            }
        }
    }

    MN1_LOG_INFO(L"[TouchOutThread] Exiting");
    return 0;
}

mn1_result_t usb_handler_start_workers(usb_device_state_t* pDev)
{
    USB_LOG_PHASE("PHASE 4 - Start Worker Threads");

    if (!pDev->bConfigured) {
        MN1_LOG_ERROR(L"  Cannot start workers: device not configured");
        return MN1_ERR_USB_NO_DEVICE;
    }

    /* Allocate double video buffers */
    pDev->pVideoRxBuf[0] = (uint8_t*)LocalAlloc(LMEM_FIXED, MN1_USB_RX_BUFFER_SIZE);
    pDev->pVideoRxBuf[1] = (uint8_t*)LocalAlloc(LMEM_FIXED, MN1_USB_RX_BUFFER_SIZE);
    if (!pDev->pVideoRxBuf[0] || !pDev->pVideoRxBuf[1]) {
        MN1_LOG_ERROR(L"  Failed to allocate video RX buffers");
        return MN1_ERR_OUT_OF_MEMORY;
    }

    pDev->nActiveVideoBuf = 0;
    InitializeCriticalSection(&pDev->csVideoSwap);

    /* Create synchronization events */
    pDev->hVideoInEvent  = CreateEvent(NULL, FALSE, FALSE, NULL);
    pDev->hTouchOutEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    pDev->bThreadsRunning = TRUE;

    /* Start Video IN thread at highest priority */
    pDev->hVideoInThread = CreateThread(
        NULL,
        MN1_STACK_SIZE,
        video_in_thread_proc,
        pDev,
        0,
        NULL
    );
    if (pDev->hVideoInThread) {
        CeSetThreadPriority(pDev->hVideoInThread, THREAD_PRIORITY_HIGHEST);
        MN1_LOG_INFO(L"  Video IN thread started: handle=0x%p (HIGHEST priority)",
                     pDev->hVideoInThread);
    } else {
        MN1_LOG_ERROR(L"  Failed to create Video IN thread: %d", GetLastError());
    }

    /* Start Touch OUT thread at above-normal priority */
    pDev->hTouchOutThread = CreateThread(
        NULL,
        MN1_STACK_SIZE,
        touch_out_thread_proc,
        pDev,
        0,
        NULL
    );
    if (pDev->hTouchOutThread) {
        CeSetThreadPriority(pDev->hTouchOutThread, THREAD_PRIORITY_ABOVE_NORMAL);
        MN1_LOG_INFO(L"  Touch OUT thread started: handle=0x%p (ABOVE_NORMAL priority)",
                     pDev->hTouchOutThread);
    } else {
        MN1_LOG_ERROR(L"  Failed to create Touch OUT thread: %d", GetLastError());
    }

    MN1_LOG_INFO(L"  Worker threads running.");
    return MN1_OK;
}

/* =========================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

void usb_handler_queue_touch(
    usb_device_state_t*     pDev,
    const mn1_touch_event_t* pEvent)
{
    pDev->pendingTouch = *pEvent;
    pDev->bTouchPending = TRUE;
    SetEvent(pDev->hTouchOutEvent);
}

const uint8_t* usb_handler_get_video_frame(
    usb_device_state_t* pDev,
    uint32_t*           pdwLen)
{
    int readBuf;

    EnterCriticalSection(&pDev->csVideoSwap);
    readBuf = pDev->nActiveVideoBuf;
    LeaveCriticalSection(&pDev->csVideoSwap);

    if (pdwLen)
        *pdwLen = pDev->dwVideoRxLen[readBuf];

    return pDev->pVideoRxBuf[readBuf];
}

/* =========================================================================
 * INIT AND SHUTDOWN
 * ========================================================================= */

mn1_result_t usb_handler_init(usb_device_state_t* pDev)
{
    mn1_result_t result;

    USB_LOG_PHASE("USB HANDLER INITIALIZATION");
    MN1_LOG_INFO(L"  Target: Au1320 MIPS, WinCE 6.0");
    MN1_LOG_INFO(L"  Mode: User-Mode USB Host Stack (no PnP, no usbser.dll)");

    memset(pDev, 0, sizeof(usb_device_state_t));

    /* Phase 0: Kill MgrUSB.exe */
    result = kill_mgrusb();
    /* Non-fatal - continue even if it wasn't running */

    /* Phase 1a: Try HCD stream driver */
    result = try_hcd_stream(pDev);
    if (result == MN1_OK) {
        MN1_LOG_INFO(L"  USB access method: HCD Stream Driver");
        return MN1_OK;
    }

    /* Phase 1b: Fall back to EHCI direct register access */
    result = try_ehci_direct(pDev);
    if (result == MN1_OK) {
        MN1_LOG_INFO(L"  USB access method: EHCI Direct Register Access (MMIO)");

        /* Initialize EHCI controller */
        MN1_LOG_INFO(L"  Initializing EHCI controller...");

        /* Reset the controller */
        pDev->pEhciOp[EHCI_OP_USBCMD / 4] = EHCI_CMD_HCRESET;
        Sleep(100);

        /* Wait for reset to complete */
        int retry = 0;
        while ((pDev->pEhciOp[EHCI_OP_USBCMD / 4] & EHCI_CMD_HCRESET) && retry < 20) {
            Sleep(10);
            retry++;
        }

        if (pDev->pEhciOp[EHCI_OP_USBCMD / 4] & EHCI_CMD_HCRESET) {
            MN1_LOG_ERROR(L"  EHCI reset timed out!");
            return MN1_ERR_USB_INIT;
        }

        MN1_LOG_INFO(L"  EHCI reset complete.");

        /* Set configured flag (routes all ports to EHCI) */
        pDev->pEhciOp[EHCI_OP_CONFIGFLAG / 4] = 1;
        Sleep(10);

        /* Enable port power */
        uint32_t portsc = pDev->pEhciOp[EHCI_OP_PORTSC0 / 4];
        portsc |= EHCI_PORTSC_PORT_POWER;
        portsc &= ~EHCI_PORTSC_WRITE_CLEAR;
        pDev->pEhciOp[EHCI_OP_PORTSC0 / 4] = portsc;

        /* Start the controller */
        uint32_t cmd = EHCI_CMD_RUN | EHCI_CMD_INT_THRESHOLD(8);
        pDev->pEhciOp[EHCI_OP_USBCMD / 4] = cmd;

        MN1_LOG_INFO(L"  EHCI controller started. USBCMD=0x%08X USBSTS=0x%08X",
                     pDev->pEhciOp[EHCI_OP_USBCMD / 4],
                     pDev->pEhciOp[EHCI_OP_USBSTS / 4]);

        return MN1_OK;
    }

    MN1_LOG_ERROR(L"  ALL USB ACCESS METHODS FAILED!");
    MN1_LOG_ERROR(L"  Neither HCD stream driver nor EHCI direct access is available.");
    MN1_LOG_ERROR(L"  Possible causes:");
    MN1_LOG_ERROR(L"    1. USB hardware not present or not powered");
    MN1_LOG_ERROR(L"    2. EHCI physical address differs from probed locations");
    MN1_LOG_ERROR(L"    3. VirtualCopy blocked by kernel policy");
    return MN1_ERR_USB_INIT;
}

void usb_handler_shutdown(usb_device_state_t* pDev)
{
    USB_LOG_PHASE("USB HANDLER SHUTDOWN");

    /* Stop worker threads */
    pDev->bThreadsRunning = FALSE;

    if (pDev->hVideoInEvent) SetEvent(pDev->hVideoInEvent);
    if (pDev->hTouchOutEvent) SetEvent(pDev->hTouchOutEvent);

    if (pDev->hVideoInThread) {
        WaitForSingleObject(pDev->hVideoInThread, 3000);
        CloseHandle(pDev->hVideoInThread);
        MN1_LOG_INFO(L"  Video IN thread stopped");
    }
    if (pDev->hTouchOutThread) {
        WaitForSingleObject(pDev->hTouchOutThread, 3000);
        CloseHandle(pDev->hTouchOutThread);
        MN1_LOG_INFO(L"  Touch OUT thread stopped");
    }

    if (pDev->hVideoInEvent) CloseHandle(pDev->hVideoInEvent);
    if (pDev->hTouchOutEvent) CloseHandle(pDev->hTouchOutEvent);

    DeleteCriticalSection(&pDev->csVideoSwap);

    /* Free video buffers */
    if (pDev->pVideoRxBuf[0]) LocalFree(pDev->pVideoRxBuf[0]);
    if (pDev->pVideoRxBuf[1]) LocalFree(pDev->pVideoRxBuf[1]);

    /* Close HCD handle */
    if (pDev->hHCD && pDev->hHCD != INVALID_HANDLE_VALUE) {
        CloseHandle(pDev->hHCD);
        MN1_LOG_INFO(L"  HCD handle closed");
    }

    /* Unmap EHCI registers */
    if (pDev->pEhciVirtBase) {
        VirtualFree(pDev->pEhciVirtBase, 0, MEM_RELEASE);
        MN1_LOG_INFO(L"  EHCI registers unmapped");
    }

    /* Log final statistics */
    MN1_LOG_INFO(L"  Statistics:");
    MN1_LOG_INFO(L"    Total bytes IN:  %d", pDev->dwTotalBytesIn);
    MN1_LOG_INFO(L"    Total bytes OUT: %d", pDev->dwTotalBytesOut);
    MN1_LOG_INFO(L"    Transfer errors: %d", pDev->dwTransferErrors);
    MN1_LOG_INFO(L"    NAK retries:     %d", pDev->dwNAKRetries);

    memset(pDev, 0, sizeof(usb_device_state_t));
    MN1_LOG_INFO(L"  Shutdown complete.");

    /* Graceful exit: bring MgrUSB.exe back so the factory radio
     * regains USB Mass Storage functionality without requiring a
     * full ECU reboot. Non-fatal if this fails. */
    usb_handler_restart_mgrusb();
}

/* =========================================================================
 * LEGACY API COMPATIBILITY WRAPPER
 * ========================================================================= */

/* Public wrappers for MgrUSB control, callable from main.cpp.
 *
 * NOTE: kill_mgrusb() is an internal static that already does the heavy
 * lifting. We simply forward to it for a stable public ABI. */
mn1_result_t usb_handler_kill_mgrusb(void)
{
    return kill_mgrusb();
}

/* Restart MgrUSB.exe after we're done with the USB hardware so the
 * factory radio regains Mass Storage functionality on a clean exit.
 *
 * We probe the standard MediaNav install locations (the binary lives
 * in the Windows directory or one of the flash partitions depending
 * on firmware revision). */
mn1_result_t usb_handler_restart_mgrusb(void)
{
    /* Candidate paths for MgrUSB.exe, in priority order */
    static const WCHAR* mgrusbPaths[] = {
        L"\\Windows\\MgrUSB.exe",
        L"\\FlashDisk\\MgrUSB.exe",
        L"\\FlashDisk\\System\\MgrUSB.exe",
        L"\\Storage Card\\MgrUSB.exe",
        L"\\Release\\MgrUSB.exe",
        L"MgrUSB.exe",            /* Search PATH */
        NULL
    };

    PROCESS_INFORMATION pi;
    int i;

    USB_LOG_PHASE("RESTART MgrUSB.exe");

    for (i = 0; mgrusbPaths[i] != NULL; i++) {
        MN1_LOG_INFO(L"  Trying CreateProcess('%s')...", mgrusbPaths[i]);

        memset(&pi, 0, sizeof(pi));

        /* WinCE CreateProcess signature: 6 args (no STARTUPINFO, no env) */
        if (CreateProcess(
                mgrusbPaths[i],
                NULL,        /* lpCommandLine */
                NULL,        /* lpProcessAttributes */
                NULL,        /* lpThreadAttributes */
                FALSE,       /* bInheritHandles */
                0,           /* dwCreationFlags */
                NULL,        /* lpEnvironment */
                NULL,        /* lpCurrentDirectory */
                NULL,        /* lpStartupInfo */
                &pi))
        {
            MN1_LOG_INFO(L"  MgrUSB.exe restarted: PID=%d (path='%s')",
                         pi.dwProcessId, mgrusbPaths[i]);
            if (pi.hProcess) CloseHandle(pi.hProcess);
            if (pi.hThread)  CloseHandle(pi.hThread);
            return MN1_OK;
        } else {
            MN1_LOG_INFO(L"  '%s' not found (err=%d)",
                         mgrusbPaths[i], GetLastError());
        }
    }

    MN1_LOG_WARN(L"  Could not restart MgrUSB.exe (factory Mass Storage may not resume until reboot)");
    return MN1_ERR_GENERIC;
}

void usb_handler_get_legacy_conn(
    const usb_device_state_t* pDev,
    mn1_usb_conn_t*           pConn)
{
    memset(pConn, 0, sizeof(mn1_usb_conn_t));

    pConn->hUSBDevice    = pDev->hHCD;
    pConn->hBulkIn       = pDev->hHCD;  /* Shared handle; endpoint selection via IOCTL */
    pConn->hBulkOut      = pDev->hHCD;
    pConn->wMaxPacketIn  = pDev->epBulkIn.wMaxPacketSize;
    pConn->wMaxPacketOut = pDev->epBulkOut.wMaxPacketSize;
    pConn->bConnected    = pDev->bConfigured && pDev->bAOAMode;
}
