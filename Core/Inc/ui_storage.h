/**
 ******************************************************************************
 * @file    ui_storage.h
 * @brief   LittleFS mount and UI directory tree management.
 *
 * Responsibilities:
 *  - Mount the LittleFS filesystem on the external NOR flash.
 *  - On first boot (blank flash), format and re-mount automatically.
 *  - Create the directory tree expected by the LVGL XML engine.
 *
 * What this module does NOT do:
 *  - It does not create or manage globals.xml. That file is authored by the
 *    C# designer tool and delivered to the device via USB upload, exactly
 *    like any other UI asset.
 *
 * Directory layout on flash (LVGL drive letter "A:"):
 *
 *   ui/
 *   ├── globals.xml         Uploaded via USB by the designer tool
 *   ├── screens/            XML file per screen
 *   ├── components/         Reusable XML component definitions
 *   ├── fonts/              Font files uploaded via USB
 *   └── images/             Image assets uploaded via USB
 ******************************************************************************
 */

#ifndef UI_STORAGE_H
#define UI_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Return codes
 * -------------------------------------------------------------------------*/
typedef enum
{
    UI_STORAGE_OK          =  0,
    UI_STORAGE_ERR_MOUNT   = -1,  /* lfs_mount failed even after format  */
    UI_STORAGE_ERR_FORMAT  = -2,  /* lfs_format failed                   */
    UI_STORAGE_ERR_DIR     = -3,  /* failed to create a directory        */
} UiStorageResult_t;

/* -------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------*/

/**
 * @brief  Mount the filesystem and ensure the UI directory tree exists.
 *
 * Call sequence:
 *  1. lfs_port_init()
 *  2. lfs_mount()  — if it fails: lfs_format() + lfs_mount()
 *  3. Create missing directories under ui/
 *
 * @note  Call once at startup, after MX_OCTOSPI1_Init(), before LVGL init.
 */
UiStorageResult_t ui_storage_init(void);

/**
 * @brief  Unmount the filesystem cleanly.
 */
void ui_storage_deinit(void);

/**
 * @brief  Returns true if the filesystem is currently mounted.
 */
bool ui_storage_is_mounted(void);

/**
 * @brief  Walk the entire filesystem tree and write the result into a static
 *         char buffer that can be inspected in the debugger without UART.
 *
 * Usage in STM32CubeIDE:
 *  1. Call ui_storage_list_tree() anywhere after ui_storage_init().
 *  2. In the Expressions window add:   ui_storage_tree_buf
 *  3. The full directory listing is visible as a string.
 *
 * @retval Pointer to the internal static buffer (null-terminated string).
 */
const char *ui_storage_list_tree(void);

/* Add this name to the debugger Expressions window to inspect the tree. */
extern char ui_storage_tree_buf[512];

#ifdef __cplusplus
}
#endif

#endif /* UI_STORAGE_H */
