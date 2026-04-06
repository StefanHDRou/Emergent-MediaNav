# Android Companion App (Concept)

## Overview

The companion app runs on the Android phone and acts as the "bridge" between
Android Auto's UI and the MediaNav MN1 hardware.

**Why is this needed?**
Because the MN1 cannot decode H.264 video at usable framerates, and Android
Auto Protocol does not support MJPEG. The companion app captures the Android
Auto screen and re-encodes it as MJPEG for the MN1 to decode.

---

## Architecture

```
+------------------------------------------------------------------+
|                      ANDROID PHONE                                |
|                                                                    |
|  +-------------------+    +------------------+    +-----------+   |
|  | Android Auto App  |--->| MediaProjection  |--->| JPEG      |   |
|  | (Google's app)    |    | API              |    | Encoder   |   |
|  +-------------------+    | (screen capture) |    | (HW accel)|   |
|                            +------------------+    +-----------+   |
|                                                         |          |
|                                                         v          |
|  +-----------+    +-----------+    +-----------------+             |
|  | Touch     |<---| USB AOA   |<---| Custom Protocol |             |
|  | Injector  |    | Device    |    | Framing         |             |
|  +-----------+    +-----------+    +-----------------+             |
+------------------------------------------------------------------+
```

---

## Key Components

### 1. Screen Capture (MediaProjection API)
```java
MediaProjectionManager mpm = getSystemService(MediaProjectionManager.class);
// Request permission via startActivityForResult
MediaProjection projection = mpm.getMediaProjection(resultCode, data);

// Create virtual display at target resolution
VirtualDisplay display = projection.createVirtualDisplay(
    "MN1-AA",
    400, 240,     // Match MN1 decode resolution
    160,           // DPI
    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
    surface,       // Surface to receive frames
    null, null
);
```

### 2. JPEG Encoding
```java
// Use Android's hardware JPEG encoder for minimal CPU usage
ImageReader reader = ImageReader.newInstance(400, 240, ImageFormat.JPEG, 2);
reader.setOnImageAvailableListener(reader -> {
    Image image = reader.acquireLatestImage();
    ByteBuffer buffer = image.getPlanes()[0].getBuffer();
    byte[] jpegData = new byte[buffer.remaining()];
    buffer.get(jpegData);
    sendToUSB(jpegData);
    image.close();
}, handler);

// Alternative: Manual YUV -> JPEG using libjpeg-turbo for quality control
```

### 3. USB AOA Device Mode
```java
UsbManager usbManager = getSystemService(UsbManager.class);
UsbAccessory[] accessories = usbManager.getAccessoryList();

// When connected to MN1 (which initiated AOA handshake):
ParcelFileDescriptor fd = usbManager.openAccessory(accessories[0]);
FileInputStream input = new FileInputStream(fd.getFileDescriptor());
FileOutputStream output = new FileOutputStream(fd.getFileDescriptor());
```

### 4. Frame Rate Control
```java
// Target 15 FPS = 66.7ms per frame
// Use a ScheduledExecutorService for precise timing
scheduler.scheduleAtFixedRate(() -> {
    captureAndSendFrame();
}, 0, 67, TimeUnit.MILLISECONDS);
```

---

## Implementation Status

**This companion app is NOT included in this repository.**

It requires a separate Android Studio project. The MN1 client code
provides the protocol specification for interoperability.

To build the companion app, implement:
1. USB AOA device listener (receive config, send JPEG frames)
2. MediaProjection screen capture at configured resolution
3. JPEG encoding at configured quality
4. Touch event reception and injection
5. Frame rate throttling

---

## Testing Without the Companion App

For development/testing of the MN1 client without the Android companion app:

1. **Static JPEG test**: Place a JPEG file on the USB stick and modify
   the MN1 client to decode it in a loop (test decode performance)

2. **USB loopback test**: Use the `tools/usb_test.c` utility to verify
   USB bulk transfer functionality

3. **Desktop emulation**: Build the MJPEG decoder for x86/Windows desktop
   (exclude WinCE-specific code) and test with JPEG files
