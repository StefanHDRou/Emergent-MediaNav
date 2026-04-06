/*
 * display.h - Display Output Abstraction
 */

#ifndef MN1AA_DISPLAY_H
#define MN1AA_DISPLAY_H

#include "config.h"
#include "types.h"

/*
 * Initialize display subsystem.
 * Creates the rendering window and sets up DirectDraw or GDI backend.
 *
 * @param hInstance  Application instance handle
 * @return MN1_OK on success
 */
mn1_result_t mn1_display_init(HINSTANCE hInstance);

/*
 * Blit a decoded frame to the display.
 *
 * For DirectDraw: Uses hardware Blt if available, or software fallback.
 * For GDI: Uses StretchDIBits for scaling.
 *
 * @param pPixels    RGB565 pixel data
 * @param wSrcWidth  Source image width
 * @param wSrcHeight Source image height
 * @return MN1_OK on success
 *
 * If source dimensions differ from display dimensions, nearest-neighbor
 * upscale is performed (cheapest scaling, no filtering overhead).
 */
mn1_result_t mn1_display_blit(
    const uint16_t* pPixels,
    uint16_t        wSrcWidth,
    uint16_t        wSrcHeight
);

/*
 * Get a pointer to the display's back buffer for direct rendering.
 * If DirectDraw is available, this locks the back surface.
 * Otherwise, returns a pointer to an internal buffer.
 *
 * @param ppPixels   Output: pointer to RGB565 back buffer
 * @param pdwStride  Output: stride in pixels (may be > display width)
 * @return MN1_OK on success
 */
mn1_result_t mn1_display_lock(uint16_t** ppPixels, uint32_t* pdwStride);

/*
 * Release the back buffer lock and flip/present.
 */
mn1_result_t mn1_display_unlock(void);

/*
 * Shutdown display and release all resources.
 */
void mn1_display_shutdown(void);

/*
 * Get the application window handle (for touch input registration).
 */
HWND mn1_display_get_hwnd(void);

#endif /* MN1AA_DISPLAY_H */
