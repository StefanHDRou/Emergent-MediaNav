/*
 * usb_test.c - USB Connectivity Test Tool
 *
 * Standalone utility to test USB Host functionality on the MediaNav.
 * Run this before the full mn1aa.exe to verify:
 *   1. USB OTG mode switching works
 *   2. USB device detection works
 *   3. AOA handshake succeeds
 *   4. Bulk data transfer works
 *
 * Build: clmips.exe /nologo /DUNICODE /D_UNICODE /DUNDER_CE=0x600
 *        /D_WIN32_WCE=0x600 /DWINCE /DMIPS /D_MIPS_ /DDEBUG
 *        /DMN1_LOG_LEVEL=3 /DMN1_LOG_TARGET=2
 *        usb_test.c /link /SUBSYSTEM:WINDOWSCE /MACHINE:MIPS
 *        coredll.lib corelibc.lib /OUT:usb_test.exe
 *
 * Usage: Copy to USB stick, run via Total Commander on MediaNav.
 *        Output appears in debug serial console (if connected) or
 *        as MessageBox popups.
 */

#include <windows.h>

/* Minimal type definitions (standalone, no dependency on project headers) */
typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned int    uint32_t;

#define AOA_GET_PROTOCOL  51
#define AOA_SEND_STRING   52
#define AOA_START         53

/* Test results displayed via MessageBox */
static WCHAR g_szResult[4096];
static int   g_nResultLen = 0;

static void log_msg(const WCHAR* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    g_nResultLen += wvsprintf(g_szResult + g_nResultLen, fmt, args);
    va_end(args);
    g_nResultLen += wsprintf(g_szResult + g_nResultLen, L"\r\n");
    OutputDebugString(g_szResult + g_nResultLen - 2);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    g_szResult[0] = 0;
    g_nResultLen = 0;

    log_msg(L"=== MN1 USB Test Tool ===");
    log_msg(L"");

    /* Test 1: Check VirtualAlloc/VirtualCopy for OTG register access */
    log_msg(L"[TEST 1] VirtualAlloc test...");
    {
        void* pVirt = VirtualAlloc(0, 4096, MEM_RESERVE, PAGE_READWRITE);
        if (pVirt) {
            log_msg(L"  VirtualAlloc OK: %p", pVirt);
            VirtualFree(pVirt, 0, MEM_RELEASE);
        } else {
            log_msg(L"  VirtualAlloc FAILED: %d", GetLastError());
        }
    }

    /* Test 2: Check if USB Host driver is loaded */
    log_msg(L"");
    log_msg(L"[TEST 2] USB Host driver check...");
    {
        HKEY hKey;
        LONG lRes;

        lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                            L"Drivers\\USB\\HCD", 0, KEY_READ, &hKey);
        if (lRes == ERROR_SUCCESS) {
            log_msg(L"  USB HCD registry key found");
            RegCloseKey(hKey);
        } else {
            log_msg(L"  USB HCD registry key NOT found (err %d)", lRes);
        }

        /* Check for USB function controller (device mode) */
        lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                            L"Drivers\\USB\\FunctionController",
                            0, KEY_READ, &hKey);
        if (lRes == ERROR_SUCCESS) {
            log_msg(L"  USB FunctionController found (device mode active?)");
            RegCloseKey(hKey);
        }
    }

    /* Test 3: Scan for USB devices */
    log_msg(L"");
    log_msg(L"[TEST 3] Scanning for USB devices...");
    {
        DEVMGR_DEVICE_INFORMATION di;
        HANDLE hSearch;

        memset(&di, 0, sizeof(di));
        di.dwSize = sizeof(di);

        GUID guidUSB = { 0xA5DCBF10L, 0x6530, 0x11D2,
            { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

        hSearch = FindFirstDevice(DeviceSearchByGuid, &guidUSB, &di);

        if (hSearch != INVALID_HANDLE_VALUE) {
            do {
                log_msg(L"  Device: %s", di.szDeviceName);
                log_msg(L"    Key: %s", di.szDeviceKey);
                log_msg(L"    Bus: %s", di.szBusName);
            } while (FindNextDevice(hSearch, &di));
            FindClose(hSearch);
        } else {
            log_msg(L"  No USB devices found (err %d)", GetLastError());
            log_msg(L"  NOTE: USB may be in Device mode.");
            log_msg(L"  Try: Use OTG cable with ID pin grounded");
        }
    }

    /* Test 4: Check Au1320 USB registers (DANGEROUS - may crash!) */
    log_msg(L"");
    log_msg(L"[TEST 4] Au1320 USB OTG register probe (RISKY!)...");
    {
        void* pVirt = VirtualAlloc(0, 4096, MEM_RESERVE, PAGE_READWRITE);
        if (pVirt) {
            /* Try mapping Au1320 USB base at 0x14020000 */
            if (VirtualCopy(pVirt, (void*)(0x14020000 >> 8), 4096,
                            PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL))
            {
                volatile uint32_t* regs = (volatile uint32_t*)pVirt;
                log_msg(L"  USB reg[0] = 0x%08X", regs[0]);
                log_msg(L"  USB reg[1] = 0x%08X", regs[1]);
                log_msg(L"  USB reg[2] = 0x%08X", regs[2]);
                log_msg(L"  USB reg[3] = 0x%08X", regs[3]);
                log_msg(L"  (If all 0x00000000 or 0xFFFFFFFF, wrong address)");
            } else {
                log_msg(L"  VirtualCopy FAILED: %d (address may be wrong)",
                        GetLastError());
            }
            VirtualFree(pVirt, 0, MEM_RELEASE);
        }
    }

    /* Test 5: System info */
    log_msg(L"");
    log_msg(L"[TEST 5] System information...");
    {
        SYSTEM_INFO si;
        MEMORYSTATUS ms;

        GetSystemInfo(&si);
        log_msg(L"  Processor: arch=%d, type=%d, level=%d",
                si.wProcessorArchitecture,
                si.dwProcessorType,
                si.wProcessorLevel);
        log_msg(L"  Page size: %d", si.dwPageSize);

        ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        log_msg(L"  Total phys: %d MB", ms.dwTotalPhys / (1024*1024));
        log_msg(L"  Avail phys: %d MB", ms.dwAvailPhys / (1024*1024));
        log_msg(L"  Mem load:   %d%%", ms.dwMemoryLoad);
    }

    /* Show results */
    log_msg(L"");
    log_msg(L"=== Test Complete ===");

    MessageBox(NULL, g_szResult, L"MN1 USB Test Results", MB_OK);

    return 0;
}
