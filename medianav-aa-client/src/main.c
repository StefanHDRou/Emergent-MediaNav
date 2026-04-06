/*
 * main.c - MN1 Android Auto Client Entry Point
 *
 * Windows CE 6.0 / MIPS (Alchemy Au1320)
 *
 * This is the main application loop implementing the Companion MJPEG
 * path (Path B). The standard AAP path (Path A) can be enabled via
 * config.h but is DOA for video on this hardware.
 *
 * APPLICATION FLOW:
 *
 *   1. Initialize display (fullscreen 800x480 window)
 *   2. Initialize USB Host (switch OTG to Host mode if needed)
 *   3. Wait for Android phone connection
 *   4. Perform AOA handshake (switch phone to accessory mode)
 *   5. Send configuration (resolution, quality, FPS)
 *   6. Enter main loop:
 *      a. USB Thread: Read JPEG frames from phone -> ring buffer
 *      b. Main Thread: Decode JPEG -> RGB565, blit to display
 *      c. Main Thread: Capture touch events -> send to phone
 *   7. Cleanup on exit
 *
 * THREADING MODEL:
 *   Main thread:  Window messages + JPEG decode + display blit
 *   USB thread:   USB bulk I/O (read JPEG, send touch)
 *
 * The main thread does decode + render because it owns the window
 * handle needed for GDI/DirectDraw operations on WinCE.
 */

#include "config.h"
#include "types.h"
#include "usb/usb_host.h"
#include "usb/aoa_handshake.h"
#include "protocol/custom_protocol.h"
#include "video/mjpeg_decoder.h"
#include "display/display.h"
#include "input/touch_input.h"
#include "util/ring_buffer.h"
#include "util/debug_log.h"

/* =========================================================================
 * GLOBAL APPLICATION STATE
 * ========================================================================= */

static mn1_app_state_t g_app;

/* Decode output buffer (RGB565, sized for decode resolution) */
static uint16_t g_decodeBuffer[MN1_DECODE_WIDTH * MN1_DECODE_HEIGHT];

/* USB receive buffers (double-buffered) */
static uint8_t g_usbRxBuf0[MN1_USB_RX_BUFFER_SIZE];
static uint8_t g_usbRxBuf1[MN1_USB_RX_BUFFER_SIZE];

/* MJPEG decoder context */
static mjpeg_context_t g_mjpegCtx;

/* Touch event queue (simple, single-slot - we only need latest event) */
static volatile mn1_touch_event_t g_pendingTouch;
static volatile mn1_bool_t        g_touchPending = FALSE;

/* =========================================================================
 * TOUCH CALLBACK
 *
 * Called from the main thread's message pump when a touch event occurs.
 * We store the event and let the USB thread send it.
 * ========================================================================= */

static void on_touch_event(const mn1_touch_event_t* pEvent, void* pUserData)
{
    mn1_touch_event_t scaled = *pEvent;

    /* Scale display coordinates to phone coordinates */
    mn1_touch_scale(&scaled, MN1_DECODE_WIDTH, MN1_DECODE_HEIGHT);

    /* Store for USB thread to send */
    g_pendingTouch = scaled;
    g_touchPending = TRUE;

    (void)pUserData;
}

/* =========================================================================
 * USB I/O THREAD
 *
 * Runs continuously reading JPEG frames from USB and writing touch events.
 * ========================================================================= */

static DWORD WINAPI usb_thread_proc(LPVOID lpParam)
{
    mn1_app_state_t* pApp = (mn1_app_state_t*)lpParam;
    uint8_t* rxBuf = g_usbRxBuf0;  /* Active receive buffer */
    uint32_t dwActual;
    mn1_result_t result;

    MN1_LOG_INFO(L"USB thread started");

    while (pApp->bRunning) {

        /* ---- Send pending touch event ---- */
        if (g_touchPending) {
            mn1_touch_event_t event = g_pendingTouch;
            g_touchPending = FALSE;

            result = mn1_custom_send_touch(&pApp->usb, &event);
            if (result != MN1_OK) {
                MN1_LOG_WARN(L"Touch send failed: %d", result);
            }
        }

        /* ---- Read next JPEG frame ---- */
        result = mn1_custom_read_video_frame(
            &pApp->usb,
            rxBuf,
            MN1_USB_RX_BUFFER_SIZE,
            &dwActual
        );

        if (result == MN1_OK && dwActual > 0) {
            /* Write JPEG data to ring buffer for decode thread */
            uint32_t written;

            /* Write frame length first (4 bytes LE) */
            uint8_t lenBuf[4];
            mn1_write_le32(lenBuf, dwActual);

            written = mn1_ring_write(&pApp->rxRing, lenBuf, 4);
            if (written == 4) {
                written = mn1_ring_write(&pApp->rxRing, rxBuf, dwActual);
                if (written < dwActual) {
                    /* Ring buffer full - frame dropped */
                    pApp->dwDropCount++;
                    MN1_LOG_WARN(L"Frame dropped (ring full), total drops: %d",
                                 pApp->dwDropCount);
                }
            } else {
                pApp->dwDropCount++;
            }

            /* Signal decode thread that data is available */
            SetEvent(pApp->hFrameReady);

        } else if (result == MN1_ERR_TIMEOUT) {
            /* No data available, just loop */
            Sleep(1);
        } else {
            /* USB error */
            MN1_LOG_ERROR(L"USB read error: %d", result);
            Sleep(100); /* Back off on errors */
        }
    }

    MN1_LOG_INFO(L"USB thread exiting");
    return 0;
}

/* =========================================================================
 * MAIN DECODE + RENDER LOOP
 * ========================================================================= */

static void main_loop(mn1_app_state_t* pApp)
{
    uint8_t jpegBuf[MN1_USB_RX_BUFFER_SIZE];
    uint32_t dwFpsCounter = 0;
    uint32_t dwFpsTick = mn1_get_tick_ms();
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

        /* Read frame from ring buffer */
        if (mn1_ring_readable(&pApp->rxRing) >= 4) {
            uint8_t lenBuf[4];
            uint32_t frameLen;

            /* Read frame length prefix */
            if (mn1_ring_peek(&pApp->rxRing, lenBuf, 4) == 4) {
                frameLen = mn1_read_le32(lenBuf);

                if (frameLen > 0 && frameLen <= MN1_USB_RX_BUFFER_SIZE &&
                    mn1_ring_readable(&pApp->rxRing) >= 4 + frameLen)
                {
                    /* Consume length prefix */
                    mn1_ring_skip(&pApp->rxRing, 4);

                    /* Read JPEG data */
                    mn1_ring_read(&pApp->rxRing, jpegBuf, frameLen);

                    /* ---- DECODE JPEG ---- */
                    mn1_result_t result = mjpeg_decode_frame(
                        &g_mjpegCtx,
                        jpegBuf,
                        frameLen,
                        &stats
                    );

                    if (result == MN1_OK) {
                        /* ---- BLIT TO DISPLAY ---- */
                        mn1_display_blit(
                            g_decodeBuffer,
                            MN1_DECODE_WIDTH,
                            MN1_DECODE_HEIGHT
                        );

                        pApp->dwFrameCount++;
                        dwFpsCounter++;

                        /* Update FPS every second */
                        {
                            uint32_t now = mn1_get_tick_ms();
                            if (now - dwFpsTick >= 1000) {
                                pApp->dwFps = dwFpsCounter;
                                dwFpsCounter = 0;
                                dwFpsTick = now;

                                MN1_LOG_INFO(
                                    L"FPS=%d, decode=%dms, frames=%d, drops=%d",
                                    pApp->dwFps,
                                    stats.dwDecodeUs / 1000,
                                    pApp->dwFrameCount,
                                    pApp->dwDropCount);
                            }
                        }

                    } else {
                        MN1_LOG_WARN(L"JPEG decode failed: %d", result);
                    }
                }
            }
        }
    }
}

/* =========================================================================
 * WINMAIN ENTRY POINT
 * ========================================================================= */

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR    lpCmdLine,
    int       nCmdShow)
{
    mn1_result_t result;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Initialize global state */
    memset(&g_app, 0, sizeof(g_app));
    g_app.bRunning = TRUE;

    /* ---- Step 1: Initialize logging ---- */
    mn1_log_init();
    MN1_LOG_INFO(L"========================================");
    MN1_LOG_INFO(L"  MN1 Android Auto Client v1.0");
    MN1_LOG_INFO(L"  Mode: %s",
#if MN1_OPERATING_MODE == MN1_MODE_COMPANION_MJPEG
        L"Companion MJPEG (Path B)"
#else
        L"Standard AAP (Path A)"
#endif
    );
    MN1_LOG_INFO(L"  Decode: %dx%d (scale %dx)",
                 MN1_DECODE_WIDTH, MN1_DECODE_HEIGHT, MN1_DECODE_SCALE);
    MN1_LOG_INFO(L"========================================");

    /* ---- Step 2: Initialize display ---- */
    result = mn1_display_init(hInstance);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"Display init failed: %d", result);
        MessageBox(NULL, L"Display init failed", L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 3: Initialize MJPEG decoder ---- */
    result = mjpeg_init(&g_mjpegCtx, g_decodeBuffer, MN1_DECODE_WIDTH);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"MJPEG decoder init failed: %d", result);
        goto cleanup;
    }

    /* ---- Step 4: Initialize ring buffer ---- */
    result = mn1_ring_init(&g_app.rxRing, 128 * 1024); /* 128KB ring */
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"Ring buffer init failed: %d", result);
        goto cleanup;
    }

    /* ---- Step 5: Initialize USB ---- */
    result = mn1_usb_init();
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"USB init failed: %d", result);
        MessageBox(NULL, L"USB init failed. Use OTG cable?",
                   L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 6: Wait for phone connection ---- */
    MN1_LOG_INFO(L"Waiting for phone connection...");
    /* Display "Waiting..." on screen */
    {
        HDC hDC = GetDC(mn1_display_get_hwnd());
        if (hDC) {
            RECT rc = { 0, 0, MN1_DISPLAY_WIDTH, MN1_DISPLAY_HEIGHT };
            SetBkColor(hDC, RGB(0, 0, 0));
            SetTextColor(hDC, RGB(255, 255, 255));
            DrawText(hDC, L"Connect Android phone via USB...",
                     -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ReleaseDC(mn1_display_get_hwnd(), hDC);
        }
    }

    result = mn1_usb_wait_device(30000, &g_app.usb); /* 30 sec timeout */
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"No phone detected: %d", result);
        MessageBox(NULL, L"No phone detected. Check USB connection.",
                   L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 7: AOA Handshake ---- */
    MN1_LOG_INFO(L"Starting AOA handshake...");
    result = mn1_aoa_handshake(&g_app.usb);
    if (result != MN1_OK) {
        MN1_LOG_ERROR(L"AOA handshake failed: %d", result);
        MessageBox(NULL, L"AOA handshake failed. Is Android Auto installed?",
                   L"MN1AA Error", MB_OK);
        goto cleanup;
    }

    /* ---- Step 8: Initialize custom protocol & send config ---- */
#if MN1_OPERATING_MODE == MN1_MODE_COMPANION_MJPEG
    {
        mn1_config_msg_t config;

        result = mn1_custom_init(&g_app.usb);
        if (result != MN1_OK) goto cleanup;

        config.wWidth    = MN1_DECODE_WIDTH;
        config.wHeight   = MN1_DECODE_HEIGHT;
        config.bQuality  = MN1_JPEG_DEFAULT_QUALITY;
        config.bMaxFps   = 15;
        config.bReserved[0] = 0;
        config.bReserved[1] = 0;

        result = mn1_custom_send_config(&g_app.usb, &config);
        if (result != MN1_OK) {
            MN1_LOG_ERROR(L"Config send failed: %d", result);
            goto cleanup;
        }
    }
#endif

    /* ---- Step 9: Initialize touch input ---- */
    result = mn1_touch_init(
        mn1_display_get_hwnd(),
        on_touch_event,
        &g_app
    );
    if (result != MN1_OK) {
        MN1_LOG_WARN(L"Touch init failed - continuing without touch");
    }

    /* ---- Step 10: Create synchronization events ---- */
    g_app.hFrameReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_app.hUsbReady   = CreateEvent(NULL, FALSE, TRUE, NULL);

    /* ---- Step 11: Start USB I/O thread ---- */
    g_app.hUsbThread = CreateThread(
        NULL,
        MN1_STACK_SIZE,
        usb_thread_proc,
        &g_app,
        0,
        NULL
    );

    if (!g_app.hUsbThread) {
        MN1_LOG_ERROR(L"Failed to create USB thread: %d", GetLastError());
        goto cleanup;
    }

    CeSetThreadPriority(g_app.hUsbThread, MN1_THREAD_PRIORITY_USB);
    CeSetThreadPriority(GetCurrentThread(), MN1_THREAD_PRIORITY_RENDER);

    /* ---- Step 12: Main decode + render loop ---- */
    MN1_LOG_INFO(L"Starting main loop");
    main_loop(&g_app);

    /* ---- Cleanup ---- */
cleanup:
    MN1_LOG_INFO(L"Shutting down...");

    g_app.bRunning = FALSE;

    /* Wait for USB thread to exit */
    if (g_app.hUsbThread) {
        SetEvent(g_app.hFrameReady); /* Wake it up */
        WaitForSingleObject(g_app.hUsbThread, 5000);
        CloseHandle(g_app.hUsbThread);
    }

    if (g_app.hFrameReady) CloseHandle(g_app.hFrameReady);
    if (g_app.hUsbReady)   CloseHandle(g_app.hUsbReady);

    mn1_touch_shutdown();
    mn1_usb_close(&g_app.usb);
    mn1_usb_shutdown();
    mn1_ring_free(&g_app.rxRing);
    mn1_display_shutdown();

    MN1_LOG_INFO(L"Total frames: %d, dropped: %d",
                 g_app.dwFrameCount, g_app.dwDropCount);
    MN1_LOG_INFO(L"Goodbye.");
    mn1_log_shutdown();

    return 0;
}
