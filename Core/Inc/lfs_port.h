/**
 ******************************************************************************
 * @file    lfs_port.h
 * @brief   LittleFS port for MX25LM51245G via OctoSPI (STM32U5)
 *
 * Exposes two things:
 *  1. The populated lfs_cfg  (ready to pass to lfs_mount)
 *  2. The lfs_t instance
 ******************************************************************************
 */

#ifndef LFS_PORT_H
#define LFS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lfs.h"   /* lfs_t, lfs_config */

/* -------------------------------------------------------------------------
 * Exposed instances — use only after lfs_port_init() has been called
 * -------------------------------------------------------------------------*/
extern lfs_t             lfs;      /* filesystem handle  */
extern struct lfs_config lfs_cfg;  /* config (visible for debugging) */

/* -------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------*/

/**
 * @brief  Initialises the callbacks and parameters of lfs_config.
 *         Does NOT mount the filesystem — that is done separately.
 *
 * @note   Call AFTER MX_OCTOSPI1_Init().
 */
void lfs_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LFS_PORT_H */
