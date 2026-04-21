/*
 * main.cpp - MN1 Android Auto Client Entry Point
 *
 * Windows CE 6.0 / MIPS (Alchemy Au1320)
 *
 * This is a C++ translation unit (compiled with clmips.exe in C++ mode)
 * so we can call the user-mode USB host stack implemented in
 * `usb/usb_handler.cpp` without extern "C" shim headaches.
 *
 * Code is kept strictly to C++98/03 for VS2005 MIPS compiler compatibility:
 *   - No `auto`, `nullptr`, range-for, lambdas, `<cstdint>`, `constexpr`.
 *   - No STL containers; raw arrays and POD structs only.
 *   - C-style casts everywhere to match the surrounding codebase.
 *
 * APPLICATION FLOW:
 *   1. kill MgrUSB.exe to free the USB port
 *   2. usb_handler_init     -> open HCD1: or map EHCI registers
 *   3. usb_handler_enumerate-> reset port, GET/SET descriptor chain
 *   4. usb_handler_aoa_handshake -> switch phone into AOA mode
 *   5. usb_handler_start_workers -> spawn HIGHEST/ABOVE_NORMAL threads
 *   6. Feed received video frames into the custom-protocol parser
 *      and MJPEG decoder, blit to display, relay touch events back.
 *   7. On exit: stop threads, close HCD, restart MgrUSB.exe.
 */

#include "config.h"
#include "types.h"
#include "usb/usb_handler.h"
#include "protocol/custom_protocol.h"
#include "video/mjpeg_decoder.h"
#include "display/display.h"
#include "input/touch_input.h"
#include "util/ring_buffer.h"
#include "util/debug_log.h"

/* =========================================================================
 * GLOBAL APPLICATION STATE
 * ========================================================================= */

static mn1_app_state_t    g_app;
static usb_device_state_t g_usb;   /* User-mode USB host state */

/* Decode output buffer (RGB565, sized for decode resolution) */
static uint16_t g_decodeBuffer[MN1_DECODE_WIDTH * MN1_DECODE_HEIGHT];

/* MJPEG decoder context */
static mjpeg_context_t g_mjpegCtx;

/* =========================================================================
 * TOUCH CALLBACK
 *
 * Called from the main thread's message pump when a touch event occurs.
 * Forwarded to the USB handler's dedicated Touch OUT worker.
 * ========================================================================= */

static void on_touch_event(const mn1_touch_event_t* pEvent, void* pUserData)
{
    mn1_touch_event_t scaled = *pEvent;

    /* Scale display coordinates to phone coordinates */
    mn1_touch_scale(&scaled, MN1_DECODE_WIDTH, MN1_DECODE_HEIGHT);

    /* Queue directly into the user-mode USB stack. The Touch OUT
     * thread (ABOVE_NORMAL) drains this slot asynchronously. */
    usb_handler_queue_touch(&g_usb, &scaled);

    (void)pUserData;
}

/* =========================================================================
 * FRAME PUMP THREAD
 *
 * The USB Video IN worker (spawned by usb_handler_start_workers) deposits
 * raw bulk-endpoint payloads into the handler's double-buffer. This
 * thread wakes on hVideoInEvent, pulls the latest buffer, feeds it into
 * the custom-protocol parser, and pushes complete JPEG frames into the
 * ring buffer for the main render loop to decode.
 * ========================================================================= */

static DWORD WINAPI frame_pump_proc(LPVOID lpParam)
{
    mn1_app_state_t* pApp = (mn1_app_state_t*)lpParam;

    MN1_LOG_INFO(L"[FramePump] Started");

    while (pApp->bRunning) {
        DWORD wait = WaitForSingleObject(g_usb.hVideoInEvent, 200);
        if (wait != WAIT_OBJECT_0) continue;

        uint32_t dwLen = 0;
        const uint8_t* pData = usb_handler_get_video_frame(&g_usb, &dwLen);
        if (!pData || dwLen < MN1_CUSTOM_HEADER_SIZE) continue;

        /* Parse the 8-byte custom-protocol header inline. We assume one
         * bulk transfer == one framed message, which is how the Android
         * companion app sends them (it writes [header][JPEG] in a single
         * call to the AOA bulk OUT pipe). */
        uint16_t wMagic   = (uint16_t)(pData[0] | (pData[1] << 8));
        uint8_t  bType    = pData[2];
        uint32_t dwPayLen = mn1_read_le32(pData + 4);

        if (wMagic != MN1_CUSTOM_MAGIC) {
            MN1_LOG_WARN(L"[FramePump] bad magic 0x%04X (len=%d)", wMagic, dwLen);
            continue;
        }
        if (bType != MN1_CUSTOM_FRAME_VIDEO) {
            /* Touch-ack / config-ack — ignore for now */
            continue;
        }
        if (dwPayLen == 0 ||
            dwPayLen > dwLen - MN1_CUSTOM_HEADER_SIZE ||
            dwPayLen > MN1_USB_RX_BUFFER_SIZE)
        {
            MN1_LOG_WARN(L"[FramePump] payload len mismatch: hdr=%d buf=%d",
                         dwPayLen, dwLen - MN1_CUSTOM_HEADER_SIZE);
            continue;
        }

        /* Push [len LE32][JPEG bytes] into the ring buffer so the main
         * decode loop can grab full frames atomically. */
        uint8_t lenBuf[4];
        mn1_write_le32(lenBuf, dwPayLen);

        if (mn1_ring_write(&pApp->rxRing, lenBuf, 4) == 4 &&
            mn1_ring_write(&pApp->rxRing, pData + MN1_CUSTOM_HEADER_SIZE,
                           dwPayLen) == dwPayLen)
        {
            SetEvent(pApp->hFrameReady);
        } else {
            pApp->dwDropCount++;
        }
    }

    MN1_LOG_INFO(L"[FramePump] Exiting");
    return 0;
}

/* =========================================================================
 * MAIN DECODE + RENDER LOOP
 * ========================================================================= */

static void main_loop(mn1_app_state_t* pApp)
{
    static uint8_t jpegBuf[MN1_USB_RX_BUFFER_SIZE];
    uint32_t dwFpsCounter = 0;
    uint32_t dwFpsTick    = mn1_get_tick_ms();
    mjpeg_stats_t stats;

    MN1_LOG_INFO(L"Entering main decode/render loop");

    while (pApp->bRunning) {
        /* Process Windows messages (touch events, WM_QUIT, etc.) */
        if (mn1_touch_process_messages()) {
            MN1_LOG_INFO(L"WM_QUIT received");
            pApp->bRunning = FALSE;
            break;
        }

        /* Wait for frame data (with timeout for message processing) */
        WaitForSingleObject(pApp->hFrameReady, 16); /* ~60Hz poll rate */

        if (mn1_ring_readable(&pApp->rxRing) < 4)
            continue;

        uint8_t lenBuf[4];
        if (mn1_ring_peek(&pApp->rxRing, lenBuf, 4) != 4)
            continue;

        uint32_t frameLen = mn1_read_le32(lenBuf);
        if (frameLen == 0 || frameLen > MN1_USB_RX_BUFFER_SIZE)
            continue;
        if (mn1_ring_readable(&pApp->rxRing) < 4 + frameLen)
            continue;

        /* Consume length prefix and read JPEG data */
        mn1_ring_skip(&pApp->rxRing, 4);
        mn1_ring_read(&pApp->rxRing, jpegBuf, frameLen);

        /* ---- DECODE JPEG ---- */
        mn1_result_t result = mjpeg_decode_frame(
            &g_mjpegCtx,
            jpegBuf,
            frameLen,
            &stats
        );
        if (result != MN1_OK) {
            MN1_LOG_WARN(L"JPEG decode failed: %d", result);
            continue;
        }

        /* ---- BLIT TO DISPLAY ---- */
        mn1_display_blit(g_decodeBuffer, MN1_DECODE_WIDTH, MN1_DECODE_HEIGHT);

        pApp->dwFrameCount++;
        dwFpsCounter++;

        /* Update FPS every second */
        uint32_t now = mn1_get_tick_ms();
        if (now - dwFpsTick >= 1000) {
            pApp->dwFps  = dwFpsCounter;
            dwFpsCounter = 0;
            dwFpsTick    = now;

            MN1_LOG_INFO(L"FPS=%d decode=%dms frames=%d drops=%d",
                         pApp->dwFps,
                         stats.dwDecodeUs / 1000,
                         pApp->dwFrameCount,
                         pApp->dwDropCount);
        }
    }
}

/* =========================================================================
 * WINMAIN ENTRY POINT
 * ========================================================================= */

extern "C" int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR    lpCmdLine,
    int       nCmdShow)
{
    mn1_result_t result;
    HANDLE       hFramePump = NULL;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Zero global state */
    memset(&g_app, 0, sizeof(g_app));
    memset(&g_usb, 0, sizeof(g_usb));
    g_app.bRunning = TRUE;

    /* ---- Step 1: Initialize logging ---- */
    mn1_log_init();
    MN1_LOG_INFO(L"========================================");
    MN1_LOG_INFO(L"  MN1 Android Auto Client v1.1 (C++)");
    MN1_LOG_INFO(L"  USB: User-Mode Host Stack (HCD1:/EHCI direct)");
    MN1_LOG_INFO(L"  Decode: %dx%d (scale %dx)",
                 MN1_DECODE_WIDTH, MN1_DECODE_HEIGHT, MN1_DECODE_SCALE);
    MN1_LOG_INFO(L"========================================");

    /* ---- Step 2: Kill MgrUSB.exe BEFORE doing anything with USB ----
     * This is critical: MgrUSB aggressively claims the USB port for
     * Mass Storage. CreateFile("HCD1:") would return ACCESS_DENIED
     * and any direct EHCI access would race against it. */
    MN1_LOG_INFO(L"Killing MgrUSB.exe to free the USB port...");
    usb_handler_kill_mgrusb();

    /* ---- Step 3: Initialize display ---- */
    result = mn1_display_init(hInstance);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"Display init failed: %d", result);
        MessageBox(NULL, L"Display init failed", L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 4: MJPEG decoder ---- */
    result = mjpeg_init(&g_mjpegCtx, g_decodeBuffer, MN1_DECODE_WIDTH);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"MJPEG decoder init failed: %d", result);
        goto cleanup;
    }

    /* ---- Step 5: Ring buffer ---- */
    result = mn1_ring_init(&g_app.rxRing, 128 * 1024);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"Ring buffer init failed: %d", result);
        goto cleanup;
    }

    /* ---- Step 6: User-mode USB host stack ---- */
    MN1_LOG_INFO(L"Bringing up user-mode USB host stack...");
    result = usb_handler_init(&g_usb);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"usb_handler_init failed: %d", result);
        MessageBox(NULL,
                   L"USB init failed. Neither HCD1: nor EHCI direct access is available.",
                   L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 7: Waiting UI + device enumeration ---- */
    {
        HDC hDC = GetDC(mn1_display_get_hwnd());
        if (hDC) {
            RECT rc;
            rc.left = 0; rc.top = 0;
            rc.right = MN1_DISPLAY_WIDTH; rc.bottom = MN1_DISPLAY_HEIGHT;
            SetBkColor(hDC, RGB(0, 0, 0));
            SetTextColor(hDC, RGB(255, 255, 255));
            DrawText(hDC, L"Connect Android phone via USB...",
                     -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ReleaseDC(mn1_display_get_hwnd(), hDC);
        }
    }

    MN1_LOG_INFO(L"Waiting for phone connection...");
    result = usb_handler_enumerate(&g_usb, 30000);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"usb_handler_enumerate failed: %d", result);
        MessageBox(NULL, L"No phone detected. Check USB connection.",
                   L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 8: AOA v2.0 handshake ---- */
    MN1_LOG_INFO(L"Starting AOA v2.0 handshake...");
    result = usb_handler_aoa_handshake(&g_usb);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"AOA handshake failed: %d", result);
        MessageBox(NULL, L"AOA handshake failed. Is Android Auto installed?",
                   L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 9: Populate legacy conn shim for custom_protocol ---- */
    usb_handler_get_legacy_conn(&g_usb, &g_app.usb);

#if MN1_OPERATING_MODE == MN1_MODE_COMPANION_MJPEG
    {
        mn1_config_msg_t config;

        result = mn1_custom_init(&g_app.usb);
        if (result != MN1_OK) goto cleanup;

        config.wWidth       = MN1_DECODE_WIDTH;
        config.wHeight      = MN1_DECODE_HEIGHT;
        config.bQuality     = MN1_JPEG_DEFAULT_QUALITY;
        config.bMaxFps      = 15;
        config.bReserved[0] = 0;
        config.bReserved[1] = 0;

        result = mn1_custom_send_config(&g_app.usb, &config);
        if (result != MN1_OK) {
            MN1_LOG_ERROR(L"Config send failed: %d", result);
            goto cleanup;
        }
    }
#endif

    /* ---- Step 10: Touch input ---- */
    result = mn1_touch_init(mn1_display_get_hwnd(), on_touch_event, &g_app);
    if (result != MN1_OK) {
        MN1_LOG_WARN(L"Touch init failed - continuing without touch");
    }

    /* ---- Step 11: Sync primitives ---- */
    g_app.hFrameReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_app.hUsbReady   = CreateEvent(NULL, FALSE, TRUE,  NULL);

    /* ---- Step 12: Start USB worker threads (inside the handler) ---- */
    result = usb_handler_start_workers(&g_usb);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"usb_handler_start_workers failed: %d", result);
        goto cleanup;
    }

    /* ---- Step 13: Start frame pump (bridges USB buf -> ring buffer) ---- */
    hFramePump = CreateThread(NULL, MN1_STACK_SIZE,
                              frame_pump_proc, &g_app, 0, NULL);
    if (hFramePump) {
        CeSetThreadPriority(hFramePump, MN1_THREAD_PRIORITY_USB);
    } else {
        MN1_LOG_ERROR(L"Failed to create frame pump thread: %d", GetLastError());
        goto cleanup;
    }

    CeSetThreadPriority(GetCurrentThread(), MN1_THREAD_PRIORITY_RENDER);

    /* ---- Step 14: Main decode + render loop ---- */
    MN1_LOG_INFO(L"Starting main loop");
    main_loop(&g_app);

cleanup:
    MN1_LOG_INFO(L"Shutting down...");
    g_app.bRunning = FALSE;

    /* Wake up frame pump thread and wait */
    if (hFramePump) {
        if (g_usb.hVideoInEvent) SetEvent(g_usb.hVideoInEvent);
        WaitForSingleObject(hFramePump, 3000);
        CloseHandle(hFramePump);
    }

    if (g_app.hFrameReady) CloseHandle(g_app.hFrameReady);
    if (g_app.hUsbReady)   CloseHandle(g_app.hUsbReady);

    mn1_touch_shutdown();

    /* usb_handler_shutdown also restarts MgrUSB.exe for a clean exit
     * so the factory radio regains Mass Storage functionality. */
    usb_handler_shutdown(&g_usb);

    mn1_ring_free(&g_app.rxRing);
    mn1_display_shutdown();

    MN1_LOG_INFO(L"Total frames: %d, dropped: %d",
                 g_app.dwFrameCount, g_app.dwDropCount);
    MN1_LOG_INFO(L"Goodbye.");
    mn1_log_shutdown();

    return 0;
}
