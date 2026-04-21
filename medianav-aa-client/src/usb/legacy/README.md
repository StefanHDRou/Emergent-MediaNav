# Legacy USB Modules (Deprecated)

These files (`usb_host.*`, `aoa_handshake.*`) were the first-generation USB / AOA
implementation that relied on the standard WinCE USB client driver model
(`USBDeviceAttach`, `RegisterClientDriverID`, `usbser.dll`).

That approach does **NOT** work on the stripped MediaNav MN1 WinCE 6.0 image
because:

- There is no PnP manager to dispatch device-attach callbacks
- `usbser.dll` is not present
- `MgrUSB.exe` exclusively claims the USB port for Mass Storage

They have been **superseded** by the user-mode USB host stack in
`../usb_handler.cpp`, which bypasses the OS driver layer entirely via
`CreateFile("HCD1:")` or direct EHCI MMIO register access.

## Status
- **NOT built** — excluded from the Makefile `ALL_SRC` list.
- Kept purely for reference / hardware-debugging fallback in case the
  user-mode host stack hits a kernel policy wall on a particular MediaNav
  firmware revision.

Do **not** re-link these into the final `.exe` — they conflict with
`usb_handler.cpp` symbols.
