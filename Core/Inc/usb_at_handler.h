/**
 ******************************************************************************
 * @file    usb_at_handler.h
 * @brief   AT command handler over USB CDC for file upload to LittleFS.
 *
 *  Supported command:
 *      AT+UPLOAD="<path/to/file>",<size>
 *
 *  Protocol:
 *      Host:    AT+UPLOAD="ui/screens/main.xml",1234\r\n
 *      Device:  +READY\r\n          (or +ERROR,<reason>\r\n)
 *      Host:    <1234 raw bytes>
 *      Host:    <4 bytes CRC32 little-endian>
 *      Device:  +OK\r\n             (or +ERROR,<reason>\r\n)
 *
 *  CRC32 is computed over: filename (no quotes) + file bytes
 *  Polynomial: 0xEDB88320 (zlib / PKZIP standard, compatible with
 *  Python's zlib.crc32() / binascii.crc32()).
 ******************************************************************************
 */

#ifndef USB_AT_HANDLER_H
#define USB_AT_HANDLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise queues and start the AT handler task.
 *         Call from MX_FREERTOS_Init().
 */
void usb_at_init(void);

/**
 * @brief  Feed one byte received over USB CDC into the handler.
 *         Safe to call from USB ISR context (CDC_Receive callback).
 *         Pushes the byte into a FreeRTOS stream buffer; never blocks.
 */
void usb_at_feed_byte_from_isr(uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* USB_AT_HANDLER_H */
