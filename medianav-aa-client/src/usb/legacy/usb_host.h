/*
 * usb_host.h - WinCE 6.0 USB Host Abstraction Layer
 *
 * Wraps the WinCE USBD API for USB Host operations.
 * The MediaNav Au1320 has a USB 2.0 + OTG controller.
 *
 * KEY CHALLENGE: The MediaNav's USB port defaults to Mass Storage mode.
 * We need to switch it to USB Host mode to act as the AOA accessory host.
 *
 * On WinCE 6.0, the USB Host stack consists of:
 *   HCD (Host Controller Driver) -> USBD.dll -> Client Drivers
 *
 * We operate at the USBD client level, using:
 *   - RegisterClientDriverID / RegisterClientSettings
 *   - OpenClientRegistryKey / USB_DRIVER notifications
 *   - TransferBulkIn / TransferBulkOut for data
 *   - IssueVendorTransfer for AOA control requests
 */

#ifndef MN1AA_USB_HOST_H
#define MN1AA_USB_HOST_H

#include "config.h"
#include "types.h"

/* =========================================================================
 * USB Host Initialization
 * ========================================================================= */

/*
 * Initialize the USB host stack.
 *
 * This function:
 * 1. Loads the USBD.dll interface
 * 2. Registers our client driver for USB device notifications
 * 3. Prepares for device enumeration
 *
 * IMPORTANT: On the MediaNav, USB may need to be switched from
 * Device/Mass-Storage mode to Host mode. This may require:
 *   - Writing to Au1320 USB OTG control registers
 *   - Or: Using an OTG cable that grounds the ID pin
 *   - Or: Modifying registry keys for USB role switching
 *
 * Returns: MN1_OK on success, MN1_ERR_USB_INIT on failure
 */
mn1_result_t mn1_usb_init(void);

/*
 * Shutdown USB host stack and release all resources.
 */
void mn1_usb_shutdown(void);

/* =========================================================================
 * Device Enumeration & Connection
 * ========================================================================= */

/*
 * Wait for a USB device to be connected.
 * This blocks until a device is detected or timeout expires.
 *
 * @param dwTimeoutMs  Timeout in milliseconds (0 = infinite)
 * @param pConn        Output: populated with device handles on success
 * @return MN1_OK if device found, MN1_ERR_USB_NO_DEVICE on timeout
 */
mn1_result_t mn1_usb_wait_device(uint32_t dwTimeoutMs, mn1_usb_conn_t* pConn);

/*
 * Check if the connected device is already in AOA mode
 * (VID=0x18D1, PID=0x2D00 or 0x2D01).
 *
 * @param pConn  USB connection handle
 * @return TRUE if device is in AOA mode
 */
mn1_bool_t mn1_usb_is_aoa_device(const mn1_usb_conn_t* pConn);

/* =========================================================================
 * Bulk Transfers
 * ========================================================================= */

/*
 * Read data from the device (bulk IN transfer).
 *
 * @param pConn        USB connection
 * @param pBuffer      Destination buffer
 * @param dwMaxBytes   Maximum bytes to read
 * @param pdwActual    Output: actual bytes transferred
 * @param dwTimeoutMs  Timeout per transfer
 * @return MN1_OK on success
 */
mn1_result_t mn1_usb_bulk_read(
    mn1_usb_conn_t* pConn,
    uint8_t*        pBuffer,
    uint32_t        dwMaxBytes,
    uint32_t*       pdwActual,
    uint32_t        dwTimeoutMs
);

/*
 * Write data to the device (bulk OUT transfer).
 *
 * @param pConn        USB connection
 * @param pBuffer      Source buffer
 * @param dwBytes      Bytes to write
 * @param pdwActual    Output: actual bytes transferred
 * @param dwTimeoutMs  Timeout per transfer
 * @return MN1_OK on success
 */
mn1_result_t mn1_usb_bulk_write(
    mn1_usb_conn_t* pConn,
    const uint8_t*  pBuffer,
    uint32_t        dwBytes,
    uint32_t*       pdwActual,
    uint32_t        dwTimeoutMs
);

/* =========================================================================
 * Vendor/Control Transfers (for AOA handshake)
 * ========================================================================= */

/*
 * Send a vendor-specific control transfer (USB_DIR_HOST_TO_DEVICE).
 * Used for AOA string sends and mode switch commands.
 *
 * @param pConn      USB connection
 * @param bRequest   Request code (e.g., 52 for SEND_STRING)
 * @param wValue     Value field
 * @param wIndex     Index field
 * @param pData      Data to send (can be NULL)
 * @param wLength    Data length
 * @return MN1_OK on success
 */
mn1_result_t mn1_usb_vendor_out(
    mn1_usb_conn_t* pConn,
    uint8_t         bRequest,
    uint16_t        wValue,
    uint16_t        wIndex,
    const uint8_t*  pData,
    uint16_t        wLength
);

/*
 * Receive a vendor-specific control transfer (USB_DIR_DEVICE_TO_HOST).
 * Used for AOA GET_PROTOCOL.
 *
 * @param pConn      USB connection
 * @param bRequest   Request code (e.g., 51 for GET_PROTOCOL)
 * @param wValue     Value field
 * @param wIndex     Index field
 * @param pData      Buffer for received data
 * @param wLength    Expected data length
 * @param pdwActual  Output: actual bytes received
 * @return MN1_OK on success
 */
mn1_result_t mn1_usb_vendor_in(
    mn1_usb_conn_t* pConn,
    uint8_t         bRequest,
    uint16_t        wValue,
    uint16_t        wIndex,
    uint8_t*        pData,
    uint16_t        wLength,
    uint32_t*       pdwActual
);

/* =========================================================================
 * USB OTG Mode Switching
 * ========================================================================= */

/*
 * Attempt to switch the Au1320's USB port from Device to Host mode.
 *
 * The Au1320 USB OTG controller has registers at a memory-mapped base.
 * Switching typically involves:
 *   1. Setting the OTG_CTRL register to force Host mode
 *   2. Or: Writing to the Au1320-specific USB configuration registers
 *
 * This is HIGHLY hardware-specific and may need reverse engineering
 * of the MediaNav BSP. The function attempts known register locations.
 *
 * @return MN1_OK if switch succeeded, MN1_ERR_GENERIC if failed
 *
 * FALLBACK: If software switching fails, use a USB OTG adapter cable
 * with the ID pin grounded (forces Host mode in hardware).
 */
mn1_result_t mn1_usb_switch_to_host_mode(void);

/*
 * Close USB connection and release pipes.
 */
void mn1_usb_close(mn1_usb_conn_t* pConn);

#endif /* MN1AA_USB_HOST_H */
