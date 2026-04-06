/*
 * touch_input.c - WinCE Touch Screen Input Implementation
 *
 * Captures touch events from the MediaNav's resistive touch screen
 * and translates them into protocol-specific touch events.
 *
 * WinCE Touch Input:
 * - The MediaNav likely uses a resistive touchscreen with a WinCE
 *   touch driver that generates standard WM_LBUTTONDOWN/UP/MOUSEMOVE.
 * - WinCE also supports WM_GESTURE for multi-touch on some devices,
 *   but the MN1's resistive panel is single-touch only.
 * - We hook into the window message loop to capture these events.
 *
 * Coordinate Mapping:
 * - WinCE provides screen coordinates (0,0 = top-left)
 * - Android Auto expects absolute coordinates in the phone's
 *   rendering resolution (e.g., 800x480)
 * - For the companion app, we scale to the capture resolution
 *
 * LATENCY: Touch -> USB -> Phone -> Response -> USB -> Display
 * Best case: ~60-80ms round trip (1 USB frame in each direction + processing)
 * This is acceptable for navigation UI but noticeable for games.
 */

#include "input/touch_input.h"
#include "util/debug_log.h"

/* =========================================================================
 * INTERNAL STATE
 * ========================================================================= */

static HWND                     s_hWnd = NULL;
static mn1_touch_callback_t     s_callback = NULL;
static void*                    s_pUserData = NULL;
static WNDPROC                  s_origWndProc = NULL;
static mn1_bool_t               s_bTouchDown = FALSE;
static uint8_t                  s_bPointerId = 0;

/* =========================================================================
 * SUBCLASSED WINDOW PROCEDURE
 *
 * We subclass the display window to intercept touch messages before
 * the default handler processes them.
 * ========================================================================= */

static LRESULT CALLBACK TouchWndProc(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    mn1_touch_event_t event;

    switch (msg) {
        case WM_LBUTTONDOWN:
            event.x         = (uint16_t)LOWORD(lParam);
            event.y         = (uint16_t)HIWORD(lParam);
            event.action    = 0;  /* ACTION_DOWN */
            event.pointerId = s_bPointerId;
            event.timestamp = (uint64_t)mn1_get_tick_ms() * 1000000ULL;

            s_bTouchDown = TRUE;

            if (s_callback)
                s_callback(&event, s_pUserData);
            return 0;

        case WM_LBUTTONUP:
            event.x         = (uint16_t)LOWORD(lParam);
            event.y         = (uint16_t)HIWORD(lParam);
            event.action    = 1;  /* ACTION_UP */
            event.pointerId = s_bPointerId;
            event.timestamp = (uint64_t)mn1_get_tick_ms() * 1000000ULL;

            s_bTouchDown = FALSE;

            if (s_callback)
                s_callback(&event, s_pUserData);
            return 0;

        case WM_MOUSEMOVE:
            if (s_bTouchDown) {
                event.x         = (uint16_t)LOWORD(lParam);
                event.y         = (uint16_t)HIWORD(lParam);
                event.action    = 2;  /* ACTION_MOVE */
                event.pointerId = s_bPointerId;
                event.timestamp = (uint64_t)mn1_get_tick_ms() * 1000000ULL;

                if (s_callback)
                    s_callback(&event, s_pUserData);
            }
            return 0;
    }

    /* Pass unhandled messages to original window procedure */
    return CallWindowProc(s_origWndProc, hWnd, msg, wParam, lParam);
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

mn1_result_t mn1_touch_init(
    HWND                    hWnd,
    mn1_touch_callback_t    callback,
    void*                   pUserData)
{
    MN1_LOG_INFO(L"Initializing touch input on HWND=%p", hWnd);

    s_hWnd      = hWnd;
    s_callback  = callback;
    s_pUserData = pUserData;

    /* Subclass the window to intercept touch messages */
    s_origWndProc = (WNDPROC)SetWindowLong(
        hWnd, GWL_WNDPROC, (LONG)TouchWndProc);

    if (!s_origWndProc) {
        MN1_LOG_ERROR(L"SetWindowLong failed: %d", GetLastError());
        return MN1_ERR_GENERIC;
    }

    MN1_LOG_INFO(L"Touch input initialized");
    return MN1_OK;
}

mn1_bool_t mn1_touch_process_messages(void)
{
    MSG msg;

    /* Process all pending messages (non-blocking) */
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return TRUE;

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return FALSE;
}

void mn1_touch_scale(
    mn1_touch_event_t*  pEvent,
    uint16_t            wPhoneWidth,
    uint16_t            wPhoneHeight)
{
    /*
     * Scale from display coordinates to phone coordinates.
     *
     * display (800x480) -> phone (wPhoneWidth x wPhoneHeight)
     *
     * Using integer math to avoid FPU:
     *   scaled_x = (event.x * phoneWidth + displayWidth/2) / displayWidth
     *
     * The "+ displayWidth/2" is for rounding.
     */
    pEvent->x = (uint16_t)(
        ((uint32_t)pEvent->x * wPhoneWidth + MN1_DISPLAY_WIDTH / 2)
        / MN1_DISPLAY_WIDTH
    );

    pEvent->y = (uint16_t)(
        ((uint32_t)pEvent->y * wPhoneHeight + MN1_DISPLAY_HEIGHT / 2)
        / MN1_DISPLAY_HEIGHT
    );
}

void mn1_touch_shutdown(void)
{
    if (s_hWnd && s_origWndProc) {
        SetWindowLong(s_hWnd, GWL_WNDPROC, (LONG)s_origWndProc);
        s_origWndProc = NULL;
    }

    s_hWnd      = NULL;
    s_callback  = NULL;
    s_pUserData = NULL;
}
