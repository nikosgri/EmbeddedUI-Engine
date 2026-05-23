/**
 ******************************************************************************
 * @file    ext_flash_test.h
 * @brief   External flash (MX25LM51245G) communication verification test.
 *
 * Tests the full read/write/verify cycle on the OctoSPI NOR flash, reporting
 * results over UART1. Run this once at startup to confirm the flash is healthy
 * before mounting LittleFS on top of it.
 ******************************************************************************
 */

#ifndef EXT_FLASH_TEST_H
#define EXT_FLASH_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Result codes
 * -------------------------------------------------------------------------*/
typedef enum
{
    FLASH_TEST_OK           = 0,   /*!< All steps passed                     */
    FLASH_TEST_JEDEC_FAIL   = 1,   /*!< Could not read / verify JEDEC ID     */
    FLASH_TEST_ERASE_FAIL   = 2,   /*!< Subsector erase timed out or failed  */
    FLASH_TEST_WRITE_FAIL   = 3,   /*!< Page-program failed or timed out     */
    FLASH_TEST_VERIFY_FAIL  = 4,   /*!< Read-back data does not match        */
} FlashTestResult_t;

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

/**
 * @brief  Run the external flash self-test.
 *
 * The function temporarily exits memory-mapped mode, performs the test
 * sequence (JEDEC ID → erase → write → verify), and always restores
 * memory-mapped mode before returning — even on failure.
 *
 * Progress and results are printed over UART1 (115200 8N1).
 *
 * @note   Call this AFTER MX_OCTOSPI1_Init() and BEFORE any LittleFS mount.
 *         Do NOT call from an ISR.
 *
 * @retval FLASH_TEST_OK  if every step succeeded.
 * @retval Other          see FlashTestResult_t for the failing step.
 */
FlashTestResult_t ext_flash_run_test(void);

#ifdef __cplusplus
}
#endif

#endif /* EXT_FLASH_TEST_H */
