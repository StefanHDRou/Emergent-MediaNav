# 04 - Touch Input Routing

## Overview

Touch input flows from the MediaNav's resistive touchscreen back to the
Android phone, where it's injected into the Android Auto UI.

```
MediaNav Touch Panel
    |
    v
WinCE Touch Driver (calibrated, screen coordinates)
    |
    v
WM_LBUTTONDOWN / WM_LBUTTONUP / WM_MOUSEMOVE
    |
    v
Our subclassed WndProc (touch_input.c)
    |
    v
Coordinate scaling (display -> phone resolution)
    |
    v
Touch event struct (12 bytes)
    |
    v
Custom protocol frame (header + payload)
    |
    v
USB Bulk OUT transfer
    |
    v
Android Companion App receives touch
    |
    v
AccessibilityService or virtual input injection
    |
    v
Android Auto UI processes touch
```

---

## Touch Event Structure

```c
typedef struct {
    uint16_t  x;           /* X coordinate (phone resolution) */
    uint16_t  y;           /* Y coordinate (phone resolution) */
    uint8_t   action;      /* 0=DOWN, 1=UP, 2=MOVE */
    uint8_t   pointerId;   /* 0 for single-touch */
    uint64_t  timestamp;   /* Nanoseconds (from GetTickCount * 1M) */
} mn1_touch_event_t;       /* Total: 14 bytes */
```

---

## Coordinate Scaling

The MediaNav display is 800x480 pixels. Our decode resolution may differ
(e.g., 400x240 for half-res mode). The phone expects touch coordinates in
its rendering resolution.

```
Scale formula (integer math, no FPU):

  phone_x = (screen_x * phone_width + display_width/2) / display_width
  phone_y = (screen_y * phone_height + display_height/2) / display_height

Example (half-res mode, display=800x480, phone=400x240):
  Touch at screen (400, 240):
    phone_x = (400 * 400 + 400) / 800 = 200
    phone_y = (240 * 240 + 240) / 480 = 120
    -> Sent as (200, 120) to phone
```

---

## For Standard AAP Path (Path A)

If using the standard AAP protocol instead of the companion MJPEG path,
touch events use the AAP `InputReport` protobuf message:

```protobuf
message InputReport {
    required uint64 timestamp = 1;
    optional TouchEvent touch_event = 3;
}

message TouchEvent {
    repeated Pointer pointer_data = 1;
    message Pointer {
        required uint32 x = 1;
        required uint32 y = 2;
        required uint32 pointer_id = 3;
    }
    optional uint32 action_index = 2;
    optional PointerAction action = 3;
}

enum PointerAction {
    ACTION_DOWN = 0;
    ACTION_UP = 1;
    ACTION_MOVED = 2;
    ACTION_POINTER_DOWN = 5;
    ACTION_POINTER_UP = 6;
}
```

This message is sent on the **Input Source Service** channel (dynamically
assigned during service discovery). The channel is opened with a
`ChannelOpenRequest` specifying the input service ID.

The AAP protocol expects coordinates in the resolution declared in the
`InputSourceService.TouchScreen` configuration during service discovery:

```protobuf
message InputSourceService {
    repeated TouchScreen touchscreen = 2;
    message TouchScreen {
        required int32 width = 1;   /* 800 */
        required int32 height = 2;  /* 480 */
        optional TouchScreenType type = 3;  /* RESISTIVE */
    }
}
```

---

## Latency Analysis

```
Event chain and estimated latencies:

Touch panel scan:          ~8ms  (resistive panel, depends on driver)
WinCE message dispatch:    ~1ms  (PeekMessage -> WndProc)
Coordinate scaling:        <1us  (integer math)
Protocol framing:          <1us  (memcpy header + payload)
USB bulk write:            ~2ms  (1 frame interval at HS)
Phone USB receive:         ~1ms
Touch injection:           ~5ms  (varies by Android version)
UI response rendering:     ~16ms (one frame at 60fps)
JPEG encode:               ~5ms  (phone GPU-accelerated)
USB transfer back:         ~2ms
MN1 decode + display:      ~20ms

TOTAL ROUND TRIP:          ~60-80ms

This means a ~4-5 frame delay at 15 FPS on the MN1 side.
Perceptually: noticeable lag when scrolling, acceptable for tapping buttons.
```

---

## WinCE Touch Driver Notes

The MediaNav uses a **resistive touchscreen**. Key characteristics:
- Single-touch only (no multitouch)
- Requires firm press (not hover)
- May require calibration (WinCE stores calibration in registry)
- Touch driver generates standard Windows mouse messages

If touch seems miscalibrated after running mn1aa.exe, the WinCE touch
calibration settings may need adjustment:
```
Registry: HKLM\HARDWARE\DEVICEMAP\TOUCH\CalibrationData
```

---

## Android Companion App: Touch Injection

The companion app receives touch events over USB and injects them into
the Android system. Two approaches:

### 1. InputManager.injectInputEvent (requires INJECT_EVENTS permission)
```java
// Requires system app or root
InputManager im = (InputManager) getSystemService(INPUT_SERVICE);
MotionEvent event = MotionEvent.obtain(...);
im.injectInputEvent(event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
```

### 2. AccessibilityService (no root required)
```java
// Register as AccessibilityService, use dispatchGesture
GestureDescription.Builder builder = new GestureDescription.Builder();
Path path = new Path();
path.moveTo(x, y);
builder.addStroke(new GestureDescription.StrokeDescription(path, 0, 50));
dispatchGesture(builder.build(), callback, null);
```

### 3. Virtual display + MediaProjection (cleanest approach)
If using MediaProjection to capture the Android Auto UI, touch events
can be injected into the virtual display's input channel directly.
This is the recommended approach for the companion app.
