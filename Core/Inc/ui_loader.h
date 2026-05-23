/**
 ******************************************************************************
 * @file    ui_loader.h
 * @brief   LVGL XML UI loader for the Riverdi display.
 ******************************************************************************
 */

#ifndef UI_LOADER_H
#define UI_LOADER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_LOADER_OK = 0,
    UI_LOADER_ERR_NO_FILES,
    UI_LOADER_ERR_FS_OPEN,         /* lv_fs_open failed -> FS driver issue   */
	UI_LOADER_ERR_GLOBALS,
    UI_LOADER_ERR_LOAD,            /* lv_xml_register failed -> XML parse    */
    UI_LOADER_ERR_CREATE_SCREEN    /* lv_xml_create failed                   */
} UiLoaderResult_t;

bool             ui_loader_has_ui(void);
UiLoaderResult_t ui_loader_render(void);
void             ui_loader_show_placeholder(void);
void             ui_loader_show_error(UiLoaderResult_t err);

#ifdef __cplusplus
}
#endif

#endif /* UI_LOADER_H */
