/*
 * display_ddraw.c - DirectDraw Display Backend
 *
 * Uses DirectDraw for hardware-accelerated blitting on WinCE 6.0.
 * Falls back to GDI StretchDIBits if DirectDraw is unavailable.
 *
 * DirectDraw on WinCE 6.0:
 * - IDirectDraw / IDirectDrawSurface interfaces
 * - Supports primary + back buffer surfaces
 * - Hardware Blt for scaling (if display driver supports it)
 * - Page flipping synchronized to vblank
 */

#include "display/display.h"
#include "util/debug_log.h"

#ifdef UNDER_CE
#include <ddraw.h>
#endif

/* =========================================================================
 * INTERNAL STATE
 * ========================================================================= */

static HWND             s_hWnd = NULL;
static HINSTANCE        s_hInstance = NULL;

#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW

/* DirectDraw objects */
static IDirectDraw*             s_pDD = NULL;
static IDirectDrawSurface*      s_pPrimary = NULL;
static IDirectDrawSurface*      s_pBackBuffer = NULL;
static mn1_bool_t               s_bDDrawInit = FALSE;

#endif

/* Fallback: software back buffer (used if DirectDraw unavailable) */
static uint16_t* s_pSoftBuffer = NULL;
static uint32_t  s_dwSoftStride = MN1_DISPLAY_WIDTH;

/* GDI bitmap info for fallback rendering */
static struct {
    BITMAPINFOHEADER bmiHeader;
    DWORD            bmiMasks[3]; /* RGB565 color masks */
} s_bmi;

/* =========================================================================
 * WINDOW CREATION
 * ========================================================================= */

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            /* Don't erase - we redraw the entire surface */
            return 1;

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

static mn1_result_t create_window(HINSTANCE hInstance)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName  = L"MN1AA";

    if (!RegisterClass(&wc)) {
        MN1_LOG_ERROR(L"RegisterClass failed: %d", GetLastError());
        return MN1_ERR_DISPLAY_INIT;
    }

    /* Create fullscreen window (no title bar, no border) */
    s_hWnd = CreateWindowEx(
        WS_EX_TOPMOST,
        L"MN1AA",
        L"MN1 Android Auto",
        WS_POPUP | WS_VISIBLE,
        0, 0,
        MN1_DISPLAY_WIDTH,
        MN1_DISPLAY_HEIGHT,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (!s_hWnd) {
        MN1_LOG_ERROR(L"CreateWindow failed: %d", GetLastError());
        return MN1_ERR_DISPLAY_INIT;
    }

    ShowWindow(s_hWnd, SW_SHOW);
    UpdateWindow(s_hWnd);

    /* Hide the cursor for clean display */
    ShowCursor(FALSE);

    s_hInstance = hInstance;
    return MN1_OK;
}

/* =========================================================================
 * DIRECTDRAW INITIALIZATION
 * ========================================================================= */

#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW

static mn1_result_t init_ddraw(void)
{
    HRESULT hr;
    DDSURFACEDESC ddsd;

    MN1_LOG_INFO(L"Initializing DirectDraw...");

    /* Create DirectDraw object */
    hr = DirectDrawCreate(NULL, &s_pDD, NULL);
    if (FAILED(hr)) {
        MN1_LOG_WARN(L"DirectDrawCreate failed: 0x%08X (falling back to GDI)", hr);
        return MN1_ERR_DISPLAY_INIT;
    }

    /* Set exclusive mode for full control */
    hr = IDirectDraw_SetCooperativeLevel(s_pDD, s_hWnd,
                                          DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE);
    if (FAILED(hr)) {
        MN1_LOG_WARN(L"SetCooperativeLevel failed: 0x%08X", hr);
        /* Try normal mode */
        hr = IDirectDraw_SetCooperativeLevel(s_pDD, s_hWnd, DDSCL_NORMAL);
        if (FAILED(hr)) {
            IDirectDraw_Release(s_pDD);
            s_pDD = NULL;
            return MN1_ERR_DISPLAY_INIT;
        }
    }

    /* Create primary surface with back buffer */
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw_CreateSurface(s_pDD, &ddsd, &s_pPrimary, NULL);
    if (FAILED(hr)) {
        MN1_LOG_WARN(L"CreateSurface (primary) failed: 0x%08X", hr);
        IDirectDraw_Release(s_pDD);
        s_pDD = NULL;
        return MN1_ERR_DISPLAY_INIT;
    }

    /* Create off-screen back buffer for double-buffering */
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    ddsd.dwWidth = MN1_DISPLAY_WIDTH;
    ddsd.dwHeight = MN1_DISPLAY_HEIGHT;

    /* Request RGB565 pixel format */
    ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    ddsd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    ddsd.ddpfPixelFormat.dwRGBBitCount = 16;
    ddsd.ddpfPixelFormat.dwRBitMask = 0xF800;
    ddsd.ddpfPixelFormat.dwGBitMask = 0x07E0;
    ddsd.ddpfPixelFormat.dwBBitMask = 0x001F;

    hr = IDirectDraw_CreateSurface(s_pDD, &ddsd, &s_pBackBuffer, NULL);
    if (FAILED(hr)) {
        MN1_LOG_WARN(L"CreateSurface (back) failed: 0x%08X", hr);
        /* Fall back to system memory buffer */
        s_pBackBuffer = NULL;
    }

    s_bDDrawInit = TRUE;
    MN1_LOG_INFO(L"DirectDraw initialized successfully");
    return MN1_OK;
}

#endif /* MN1_DISPLAY_DDRAW */

/* =========================================================================
 * GDI FALLBACK INITIALIZATION
 * ========================================================================= */

static mn1_result_t init_gdi_fallback(void)
{
    MN1_LOG_INFO(L"Using GDI fallback rendering");

    /* Allocate software back buffer */
    s_pSoftBuffer = (uint16_t*)LocalAlloc(LMEM_FIXED,
        MN1_DISPLAY_WIDTH * MN1_DISPLAY_HEIGHT * sizeof(uint16_t));

    if (!s_pSoftBuffer) {
        MN1_LOG_ERROR(L"Failed to allocate software back buffer");
        return MN1_ERR_OUT_OF_MEMORY;
    }

    /* Set up BITMAPINFO for RGB565 */
    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth         = MN1_DISPLAY_WIDTH;
    s_bmi.bmiHeader.biHeight        = -(int)MN1_DISPLAY_HEIGHT; /* Top-down */
    s_bmi.bmiHeader.biPlanes        = 1;
    s_bmi.bmiHeader.biBitCount      = 16;
    s_bmi.bmiHeader.biCompression   = BI_BITFIELDS;
    s_bmi.bmiHeader.biSizeImage     = MN1_FRAME_SIZE_BYTES;

    /* RGB565 masks */
    s_bmi.bmiMasks[0] = 0xF800; /* Red */
    s_bmi.bmiMasks[1] = 0x07E0; /* Green */
    s_bmi.bmiMasks[2] = 0x001F; /* Blue */

    s_dwSoftStride = MN1_DISPLAY_WIDTH;
    return MN1_OK;
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

mn1_result_t mn1_display_init(HINSTANCE hInstance)
{
    mn1_result_t result;

    result = create_window(hInstance);
    if (result != MN1_OK)
        return result;

#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW
    result = init_ddraw();
    if (result != MN1_OK) {
        MN1_LOG_WARN(L"DirectDraw init failed, using GDI fallback");
    }
#endif

    /* Always initialize GDI fallback (used if DirectDraw unavailable) */
    if (!s_bDDrawInit) {
        result = init_gdi_fallback();
    }

    return result;
}

mn1_result_t mn1_display_blit(
    const uint16_t* pPixels,
    uint16_t        wSrcWidth,
    uint16_t        wSrcHeight)
{
#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW
    if (s_bDDrawInit && s_pPrimary) {
        HRESULT hr;
        DDSURFACEDESC ddsd;
        RECT srcRect, dstRect;

        srcRect.left = 0; srcRect.top = 0;
        srcRect.right = wSrcWidth; srcRect.bottom = wSrcHeight;

        dstRect.left = 0; dstRect.top = 0;
        dstRect.right = MN1_DISPLAY_WIDTH; dstRect.bottom = MN1_DISPLAY_HEIGHT;

        if (s_pBackBuffer) {
            /* Lock back buffer, copy pixels, unlock, then Blt to primary */
            memset(&ddsd, 0, sizeof(ddsd));
            ddsd.dwSize = sizeof(ddsd);

            hr = IDirectDrawSurface_Lock(s_pBackBuffer, NULL, &ddsd,
                                          DDLOCK_WRITEONLY | DDLOCK_WAIT, NULL);
            if (SUCCEEDED(hr)) {
                /* Copy pixels to back buffer */
                uint16_t* pDst = (uint16_t*)ddsd.lpSurface;
                uint32_t dwDstStride = ddsd.lPitch / 2;
                uint16_t y;

                for (y = 0; y < wSrcHeight && y < MN1_DISPLAY_HEIGHT; y++) {
                    memcpy(pDst + y * dwDstStride,
                           pPixels + y * wSrcWidth,
                           wSrcWidth * sizeof(uint16_t));
                }

                IDirectDrawSurface_Unlock(s_pBackBuffer, NULL);

                /* Blt from back buffer to primary (handles scaling) */
                hr = IDirectDrawSurface_Blt(
                    s_pPrimary, &dstRect,
                    s_pBackBuffer, &srcRect,
                    DDBLT_WAIT, NULL);

                if (SUCCEEDED(hr))
                    return MN1_OK;
            }
        }

        /* DirectDraw Blt failed, fall through to GDI */
        MN1_LOG_WARN(L"DirectDraw Blt failed, using GDI");
    }
#endif

    /* GDI fallback: StretchDIBits */
    {
        HDC hDC = GetDC(s_hWnd);
        if (hDC) {
            /* Update the bitmap info for source dimensions */
            s_bmi.bmiHeader.biWidth  = wSrcWidth;
            s_bmi.bmiHeader.biHeight = -(int)wSrcHeight;
            s_bmi.bmiHeader.biSizeImage = wSrcWidth * wSrcHeight * 2;

            StretchDIBits(
                hDC,
                0, 0, MN1_DISPLAY_WIDTH, MN1_DISPLAY_HEIGHT,  /* Dest */
                0, 0, wSrcWidth, wSrcHeight,                   /* Source */
                pPixels,
                (BITMAPINFO*)&s_bmi,
                DIB_RGB_COLORS,
                SRCCOPY
            );

            ReleaseDC(s_hWnd, hDC);
        }
    }

    return MN1_OK;
}

mn1_result_t mn1_display_lock(uint16_t** ppPixels, uint32_t* pdwStride)
{
#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW
    if (s_bDDrawInit && s_pBackBuffer) {
        DDSURFACEDESC ddsd;
        HRESULT hr;

        memset(&ddsd, 0, sizeof(ddsd));
        ddsd.dwSize = sizeof(ddsd);

        hr = IDirectDrawSurface_Lock(s_pBackBuffer, NULL, &ddsd,
                                      DDLOCK_WRITEONLY | DDLOCK_WAIT, NULL);
        if (SUCCEEDED(hr)) {
            *ppPixels = (uint16_t*)ddsd.lpSurface;
            *pdwStride = ddsd.lPitch / 2;
            return MN1_OK;
        }
    }
#endif

    /* Software buffer fallback */
    if (s_pSoftBuffer) {
        *ppPixels = s_pSoftBuffer;
        *pdwStride = s_dwSoftStride;
        return MN1_OK;
    }

    return MN1_ERR_DISPLAY_INIT;
}

mn1_result_t mn1_display_unlock(void)
{
#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW
    if (s_bDDrawInit && s_pBackBuffer) {
        IDirectDrawSurface_Unlock(s_pBackBuffer, NULL);

        /* Blt to primary */
        RECT dstRect = { 0, 0, MN1_DISPLAY_WIDTH, MN1_DISPLAY_HEIGHT };
        IDirectDrawSurface_Blt(s_pPrimary, &dstRect,
                                s_pBackBuffer, &dstRect,
                                DDBLT_WAIT, NULL);
        return MN1_OK;
    }
#endif

    /* GDI path: blit the software buffer */
    if (s_pSoftBuffer) {
        return mn1_display_blit(s_pSoftBuffer,
                                MN1_DISPLAY_WIDTH, MN1_DISPLAY_HEIGHT);
    }

    return MN1_OK;
}

void mn1_display_shutdown(void)
{
#if MN1_DISPLAY_BACKEND == MN1_DISPLAY_DDRAW
    if (s_pBackBuffer) {
        IDirectDrawSurface_Release(s_pBackBuffer);
        s_pBackBuffer = NULL;
    }
    if (s_pPrimary) {
        IDirectDrawSurface_Release(s_pPrimary);
        s_pPrimary = NULL;
    }
    if (s_pDD) {
        IDirectDraw_Release(s_pDD);
        s_pDD = NULL;
    }
    s_bDDrawInit = FALSE;
#endif

    if (s_pSoftBuffer) {
        LocalFree(s_pSoftBuffer);
        s_pSoftBuffer = NULL;
    }

    if (s_hWnd) {
        DestroyWindow(s_hWnd);
        s_hWnd = NULL;
    }

    if (s_hInstance) {
        UnregisterClass(L"MN1AA", s_hInstance);
        s_hInstance = NULL;
    }
}

HWND mn1_display_get_hwnd(void)
{
    return s_hWnd;
}
