/**
  ******************************************************************************
  * @file    usbd_conf.c
  * @brief   USB Device library BSP for STM32U5G9 OTG_HS.
  *
  * Derived from the ST classic-coremw-apps NUCLEO-U575ZI-Q CDC example.
  * Minimal changes applied:
  *   - USB_OTG_FS  → USB_OTG_HS
  *   - OTG_FS_IRQn → OTG_HS_IRQn
  *   - __HAL_RCC_USB_CLK_ENABLE → USB_OTG_HS + USBPHYC clocks
  *   - HAL_Delay override removed (FreeRTOS owns the tick)
  *   - USB_PWR_EN pin driven in USBD_LL_Start / USBD_LL_Stop
  ******************************************************************************
  */

#include "stm32u5xx.h"
#include "stm32u5xx_hal.h"
#include "usbd_cdc.h"
#include "main.h"

PCD_HandleTypeDef hpcd_USB_OTG_HS;

static USBD_StatusTypeDef USBD_Get_USB_Status(HAL_StatusTypeDef hal_status);

/* =========================================================================
 * MSP Init / DeInit
 * =========================================================================*/
void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
    GPIO_InitTypeDef         GPIO_InitStruct      = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct  = {0};

    if (pcdHandle->Instance == USB_OTG_HS)
    {
        /* 1. Enable PWR & SYSCFG clocks FIRST — needed by everything below */
        __HAL_RCC_PWR_CLK_ENABLE();
        __HAL_RCC_SYSCFG_CLK_ENABLE();

        /* 2. Enable VDDUSB independent supply (must be on before PHY powers up) */
        HAL_PWREx_EnableVddUSB();

        /* 3. Enable USB HS transceiver supply */
        HAL_PWREx_EnableUSBHSTranceiverSupply();

        /* 4. Tell SYSCFG that PHY reference clock is 16 MHz (matches HSE) */
        HAL_SYSCFG_SetOTGPHYReferenceClockSelection(SYSCFG_OTG_HS_PHY_CLK_SELECT_1);

        /* 5. Select HSE as USBPHY clock source */
        PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USBPHY;
        PeriphClkInitStruct.UsbPhyClockSelection = RCC_USBPHYCLKSOURCE_HSE;
        if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
        {
            Error_Handler();
        }

        /* 6. Configure PA11 (DM) and PA12 (DP) — alternate function */
        __HAL_RCC_GPIOA_CLK_ENABLE();
        GPIO_InitStruct.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF10_USB_HS;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* 7. Enable the OTG PHY itself */
        HAL_SYSCFG_EnableOTGPHY(SYSCFG_OTG_HS_PHY_ENABLE);

        /* 8. Enable PHY controller and OTG_HS peripheral clocks */
        __HAL_RCC_USBPHYC_CLK_ENABLE();
        __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

        /* 9. NVIC */
        HAL_NVIC_SetPriority(OTG_HS_IRQn, 6, 0);
        HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
    }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_OTG_HS)
    {
        __HAL_RCC_USBPHYC_CLK_DISABLE();
        __HAL_RCC_USB_OTG_HS_CLK_DISABLE();
        HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
    }
}

/* =========================================================================
 * HAL PCD callbacks → USB Device library
 * =========================================================================*/

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                         hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                        hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_SpeedTypeDef speed = USBD_SPEED_HIGH;

    if (hpcd->Init.speed == PCD_SPEED_FULL)
    {
        speed = USBD_SPEED_FULL;
    }

    USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, speed);
    USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

/* =========================================================================
 * USBD_LL_* → HAL PCD
 * =========================================================================*/

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    hpcd_USB_OTG_HS.pData = pdev;
    pdev->pData = &hpcd_USB_OTG_HS;

    hpcd_USB_OTG_HS.Instance                     = USB_OTG_HS;
    hpcd_USB_OTG_HS.Init.dev_endpoints            = 9;
    hpcd_USB_OTG_HS.Init.speed                    = PCD_SPEED_HIGH;
    hpcd_USB_OTG_HS.Init.phy_itface               = USB_OTG_HS_EMBEDDED_PHY;
    hpcd_USB_OTG_HS.Init.Sof_enable               = DISABLE;
    hpcd_USB_OTG_HS.Init.low_power_enable         = DISABLE;
    hpcd_USB_OTG_HS.Init.lpm_enable               = DISABLE;
    hpcd_USB_OTG_HS.Init.battery_charging_enable  = DISABLE;
    hpcd_USB_OTG_HS.Init.use_dedicated_ep1        = DISABLE;
    hpcd_USB_OTG_HS.Init.vbus_sensing_enable      = DISABLE;

    if (HAL_PCD_Init(&hpcd_USB_OTG_HS) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_HS, 0x200);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 0, 0x40);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 1, 0x80);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
    return USBD_Get_USB_Status(HAL_PCD_DeInit(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
    return USBD_Get_USB_Status(HAL_PCD_Start(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
    HAL_GPIO_WritePin(USB_PWR_EN_GPIO_Port, USB_PWR_EN_Pin, GPIO_PIN_RESET);
    return USBD_Get_USB_Status(HAL_PCD_Stop(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                   uint8_t ep_type, uint16_t ep_mps)
{
    return USBD_Get_USB_Status(
        HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type));
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return USBD_Get_USB_Status(HAL_PCD_EP_Close(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return USBD_Get_USB_Status(HAL_PCD_EP_Flush(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return USBD_Get_USB_Status(HAL_PCD_EP_SetStall(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return USBD_Get_USB_Status(HAL_PCD_EP_ClrStall(pdev->pData, ep_addr));
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;

    if ((ep_addr & 0x80) == 0x80)
        return hpcd->IN_ep[ep_addr & 0x7F].is_stall;
    else
        return hpcd->OUT_ep[ep_addr & 0x7F].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
    return USBD_Get_USB_Status(HAL_PCD_SetAddress(pdev->pData, dev_addr));
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                     uint8_t *pbuf, uint32_t size)
{
    return USBD_Get_USB_Status(
        HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size));
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                           uint8_t *pbuf, uint32_t size)
{
    return USBD_Get_USB_Status(
        HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size));
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef *)pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
    HAL_Delay(Delay);
}

void *USBD_static_malloc(uint32_t size)
{
    static uint32_t mem[(sizeof(USBD_CDC_HandleTypeDef) / 4) + 1];
    (void)size;
    return mem;
}

void USBD_static_free(void *p)
{
    (void)p;
}

static USBD_StatusTypeDef USBD_Get_USB_Status(HAL_StatusTypeDef hal_status)
{
    switch (hal_status)
    {
        case HAL_OK:      return USBD_OK;
        case HAL_ERROR:   return USBD_FAIL;
        case HAL_BUSY:    return USBD_BUSY;
        case HAL_TIMEOUT: return USBD_FAIL;
        default:          return USBD_FAIL;
    }
}
