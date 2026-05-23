/**
 ******************************************************************************
 * @file    usbd_cdc_if.c
 * @brief   CDC interface — receives USB data and forwards it to the AT
 *          command handler.
 *
 * Unlike the ST example, this file has NO UART bridge.
 * All data received over USB is passed directly to at_handler_feed().
 ******************************************************************************
 */

#include "usbd_cdc_if.h"
#include "usb_at_handler.h"

/* -------------------------------------------------------------------------
 * Receive buffer
 * The USB library writes incoming data here via USBD_CDC_SetRxBuffer.
 * Size must be a power of 2 and >= CDC max packet size (64 bytes FS).
 * -------------------------------------------------------------------------*/
#define APP_RX_DATA_SIZE  512U

static uint8_t UserRxBuffer[APP_RX_DATA_SIZE];

/* USB device handle — defined in usb_device.c */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* -------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------*/
static int8_t CDC_Init(void);
static int8_t CDC_DeInit(void);
static int8_t CDC_Control(uint8_t cmd, uint8_t *pbuf, uint16_t length);
static int8_t CDC_Receive(uint8_t *pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_CDC_fops =
{
    CDC_Init,
    CDC_DeInit,
    CDC_Control,
    CDC_Receive,
    CDC_TransmitCplt
};

/* =========================================================================
 * CDC interface callbacks
 * =========================================================================*/

static int8_t CDC_Init(void)
{
    /* Point the library at our receive buffer */
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBuffer);
    return USBD_OK;
}

static int8_t CDC_DeInit(void)
{
    return USBD_OK;
}

/**
 * @brief  Handle CDC control requests (SET_LINE_CODING etc.).
 *         We accept everything but don't act on baud/stop/parity
 *         because there is no physical UART on the other side.
 */
static int8_t CDC_Control(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
    UNUSED(length);

    switch (cmd)
    {
        case CDC_GET_LINE_CODING:
            /* Report 115200 8N1 to keep the host happy */
            pbuf[0] = 0x00; pbuf[1] = 0xC2; pbuf[2] = 0x01; pbuf[3] = 0x00;
            pbuf[4] = 0x00; /* 1 stop bit  */
            pbuf[5] = 0x00; /* no parity   */
            pbuf[6] = 0x08; /* 8 data bits */
            break;

        default:
            break;
    }

    return USBD_OK;
}

/**
 * @brief  Data received over USB OUT endpoint.
 *
 * Called from the USB interrupt context. Passes every received byte to
 * at_handler_feed() which drives the AT command state machine.
 * After processing, re-arms the endpoint for the next packet.
 */
static int8_t CDC_Receive(uint8_t *Buf, uint32_t *Len)
{
    /* Feed each byte into the AT command state machine */
    for (uint32_t i = 0; i < *Len; i++)
    {
        //at_handler_feed(Buf[i]);
    	usb_at_feed_byte_from_isr(Buf[i]);
    }

    /* Re-arm the OUT endpoint for the next USB packet */
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBuffer);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);

    return USBD_OK;
}

static int8_t CDC_TransmitCplt(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
    UNUSED(Buf);
    UNUSED(Len);
    UNUSED(epnum);
    return USBD_OK;
}

/* =========================================================================
 * Public transmit helper
 * =========================================================================*/

/**
 * @brief  Send a buffer over USB CDC.
 *
 * Safe to call from a FreeRTOS task. Do not call from ISR context.
 *
 * @param  buf   Pointer to data to send.
 * @param  len   Number of bytes to send.
 * @retval USBD_OK on success, USBD_BUSY if the endpoint is still transmitting.
 */
USBD_StatusTypeDef CDC_Transmit(uint8_t *buf, uint16_t len)
{
    USBD_CDC_HandleTypeDef *hcdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;

    if (hcdc == NULL)
    {
        return USBD_FAIL;
    }

    if (hcdc->TxState != 0)
    {
        return USBD_BUSY;
    }

    USBD_CDC_SetTxBuffer(&hUsbDeviceFS, buf, len);
    return (USBD_StatusTypeDef)USBD_CDC_TransmitPacket(&hUsbDeviceFS);
}
