/*
 * touch_input.h - Touch Screen Input Handler
 */

#ifndef MN1AA_TOUCH_INPUT_H
#define MN1AA_TOUCH_INPUT_H

#include "config.h"
#include "types.h"

/* Touch event callback function type */
typedef void (*mn1_touch_callback_t)(const mn1_touch_event_t* pEvent, void* pUserData);

/*
 * Initialize touch input handling.
 * Registers for WM_LBUTTONDOWN/UP/MOVE messages on the display window.
 *
 * @param hWnd       Window handle to capture touch from
 * @param callback   Function called for each touch event
 * @param pUserData  User data passed to callback
 * @return MN1_OK on success
 */
mn1_result_t mn1_touch_init(
    HWND                    hWnd,
    mn1_touch_callback_t    callback,
    void*                   pUserData
);

/*
 * Process pending Windows messages (must be called in the main loop).
 * Dispatches WM_LBUTTON* messages to the touch callback.
 *
 * @return TRUE if a WM_QUIT was received
 */
mn1_bool_t mn1_touch_process_messages(void);

/*
 * Scale touch coordinates from display space to the phone's
 * expected coordinate space.
 *
 * The phone expects touch coordinates in the resolution it's
 * rendering at (e.g., 800x480 for AAP, or the companion app's
 * capture resolution).
 *
 * @param pEvent       Touch event (modified in-place)
 * @param wPhoneWidth  Phone's expected width
 * @param wPhoneHeight Phone's expected height
 */
void mn1_touch_scale(
    mn1_touch_event_t*  pEvent,
    uint16_t            wPhoneWidth,
    uint16_t            wPhoneHeight
);

/*
 * Shutdown touch input.
 */
void mn1_touch_shutdown(void);

#endif /* MN1AA_TOUCH_INPUT_H */
