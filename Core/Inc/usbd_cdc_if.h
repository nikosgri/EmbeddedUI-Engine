/**
 ******************************************************************************
 * @file    usbd_cdc_if.h
 * @brief   CDC interface header.
 ******************************************************************************
 */

#ifndef __USBD_CDC_IF_H
#define __USBD_CDC_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_cdc.h"

/* CDC interface callbacks registered with the USB library */
extern USBD_CDC_ItfTypeDef USBD_CDC_fops;

/**
 * @brief  Send data over USB CDC.
 * @param  buf  Data buffer.
 * @param  len  Number of bytes to send.
 * @retval USBD_OK / USBD_BUSY / USBD_FAIL
 */
USBD_StatusTypeDef CDC_Transmit(uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CDC_IF_H */
