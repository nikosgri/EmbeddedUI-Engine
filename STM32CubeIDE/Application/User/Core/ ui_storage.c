/**
 ******************************************************************************
 * @file    ui_storage.c
 * @brief   LittleFS mount and UI directory tree management.
 ******************************************************************************
 */

#include "ui_storage.h"
#include "lfs_port.h"   /* lfs, lfs_cfg, lfs_port_init() */
#include "lfs.h"
#include <stdio.h>      /* snprintf */

/* -------------------------------------------------------------------------
 * Directory tree required by the LVGL XML engine.
 * Every path here will be created if it does not already exist.
 *
 * globals.xml is intentionally NOT created here — it is authored by the
 * C# designer tool and uploaded to the device via the USB AT protocol,
 * the same way as any other UI file.
 * -------------------------------------------------------------------------*/
static const char * const UI_DIRS[] =
{
    "ui",
    "ui/screens",
    "ui/components",
    "ui/fonts",
    "ui/images",
};

#define UI_DIRS_COUNT  (sizeof(UI_DIRS) / sizeof(UI_DIRS[0]))

/* -------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------*/
static bool s_mounted = false;

/* Buffer written by ui_storage_list_tree().
 * Declared extern in the header so the debugger can find it by name.      */
char ui_storage_tree_buf[512];

/* =========================================================================
 * Internal helpers
 * =========================================================================*/

/**
 * @brief  Create a directory only if it does not already exist.
 *
 * lfs_mkdir returns LFS_ERR_EXIST when the directory is already there.
 * That is not an error for our purposes — treated as OK.
 */
static int _mkdir_if_missing(const char *path)
{
    int err = lfs_mkdir(&lfs, path);

    if (err == LFS_ERR_EXIST)
    {
        return LFS_ERR_OK;
    }

    return err;
}

/**
 * @brief  Create every directory in UI_DIRS that does not yet exist.
 */
static UiStorageResult_t _create_ui_tree(void)
{
    for (size_t i = 0; i < UI_DIRS_COUNT; i++)
    {
        int err = _mkdir_if_missing(UI_DIRS[i]);
        if (err != LFS_ERR_OK)
        {
            return UI_STORAGE_ERR_DIR;
        }
    }

    return UI_STORAGE_OK;
}

/**
 * @brief  Recursively list one directory into ui_storage_tree_buf.
 */
static char *_list_dir(const char *path, int depth, char *pos, char * const buf_end)
{
    lfs_dir_t       dir;
    struct lfs_info info;

    if (lfs_dir_open(&lfs, &dir, path) < 0)
    {
        return pos;
    }

    while (lfs_dir_read(&lfs, &dir, &info) > 0)
    {
        /* LittleFS always returns "." and ".." — skip them */
        if (info.name[0] == '.')
        {
            continue;
        }

        /* Indentation: two spaces per depth level */
        for (int i = 0; i < depth * 2; i++)
        {
            if (pos < buf_end - 1) { *pos++ = ' '; }
        }

        /* Tag and name */
        const char *tag = (info.type == LFS_TYPE_DIR) ? "[D] " : "[F] ";
        for (const char *c = tag; *c && pos < buf_end - 1; c++)
        {
            *pos++ = *c;
        }
        for (const char *c = info.name; *c && pos < buf_end - 1; c++)
        {
            *pos++ = *c;
        }

        /* File size for regular files */
        if (info.type == LFS_TYPE_REG)
        {
            char size_str[24];
            int  len = snprintf(size_str, sizeof(size_str),
                                "  (%lu B)", (unsigned long)info.size);
            for (int i = 0; i < len && pos < buf_end - 1; i++)
            {
                *pos++ = size_str[i];
            }
        }

        if (pos < buf_end - 1) { *pos++ = '\n'; }

        /* Recurse into subdirectories */
        if (info.type == LFS_TYPE_DIR)
        {
            char sub_path[LFS_NAME_MAX+128];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", path, info.name);
            pos = _list_dir(sub_path, depth + 1, pos, buf_end);
        }
    }

    lfs_dir_close(&lfs, &dir);
    return pos;
}

/* =========================================================================
 * Public API
 * =========================================================================*/

UiStorageResult_t ui_storage_init(void)
{
    /* Step 1 — initialise port callbacks and flash geometry */
    lfs_port_init();

    /* Step 2 — attempt to mount an existing filesystem */
    int err = lfs_mount(&lfs, &lfs_cfg);

    if (err != LFS_ERR_OK)
    {
        /* Mount failed — expected on first boot with a blank flash.
         * Format and retry. This branch is NOT taken on subsequent boots
         * so existing files are never erased unintentionally.            */
        err = lfs_format(&lfs, &lfs_cfg);
        if (err != LFS_ERR_OK)
        {
            return UI_STORAGE_ERR_FORMAT;
        }

        err = lfs_mount(&lfs, &lfs_cfg);
        if (err != LFS_ERR_OK)
        {
            return UI_STORAGE_ERR_MOUNT;
        }
    }

    s_mounted = true;

    /* Step 3 — ensure the UI directory tree exists */
    UiStorageResult_t result = _create_ui_tree();
    if (result != UI_STORAGE_OK)
    {
        lfs_unmount(&lfs);
        s_mounted = false;
        return result;
    }

    /* Hand the mounted LittleFS instance over to LVGL's FS driver so that
     * paths starting with "A:" can be opened via lv_fs_open and friends.
     * This is required by the LVGL XML loader. */
#if LV_USE_FS_LITTLEFS
    lv_littlefs_set_handler(&lfs);
#endif

    return UI_STORAGE_OK;
}

void ui_storage_deinit(void)
{
    if (s_mounted)
    {
        lfs_unmount(&lfs);
        s_mounted = false;
    }
}

bool ui_storage_is_mounted(void)
{
    return s_mounted;
}

const char *ui_storage_list_tree(void)
{
    char * const buf_end = ui_storage_tree_buf + sizeof(ui_storage_tree_buf);
    char *pos = ui_storage_tree_buf;

    const char *header = "=== Flash filesystem tree ===\n";
    for (const char *c = header; *c && pos < buf_end - 1; c++)
    {
        *pos++ = *c;
    }

    if (s_mounted)
    {
        pos = _list_dir("/", 0, pos, buf_end);
    }
    else
    {
        const char *msg = "(not mounted)\n";
        for (const char *c = msg; *c && pos < buf_end - 1; c++)
        {
            *pos++ = *c;
        }
    }

    *pos = '\0';
    return ui_storage_tree_buf;
}

