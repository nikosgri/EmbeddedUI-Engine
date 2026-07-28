///**
// ******************************************************************************
// * @file    ui_loader.c
// * @brief   Generic LVGL XML UI loader (Official 2026 Manual Compliant)
// ******************************************************************************
// */
//
//#include "ui_loader.h"
//#include "lfs.h"
//#include "lfs_port.h"
//#include "lvgl/lvgl.h"
//
//#include <stdio.h>
//#include <string.h>
//#include <stdlib.h>
//
///* =========================================================================
// * Conventions
// * =========================================================================*/
//
//#define STARTUP_SCREEN_NAME     "main_screen"
//
//#define UI_GLOBALS_LFS_PATH     "ui/globals.xml"
//#define UI_COMPONENTS_DIR       "ui/components"
//#define UI_SCREENS_DIR          "ui/screens"
//
///* =========================================================================
// * Diagnostic state
// * =========================================================================*/
//
//static int s_globals_register_err   = -99;
//static int s_components_walked      = 0;
//static int s_components_registered  = 0;
//static int s_screens_walked         = 0;
//static int s_screens_registered     = 0;
//static int s_create_screen_failed   = 0;
//
///* =========================================================================
// * Private helpers
// * =========================================================================*/
//
//static bool _is_xml          (const char *name);
//static int  _count_xmls_in   (const char *dir_path);
//static void _walk_and_register(const char *dir_path,
//                                int        *out_walked,
//                                int        *out_registered);
//static lv_result_t _safe_register_from_file(const char *component_name, const char *lfs_path);
//
///* =========================================================================
// * Public API
// * =========================================================================*/
//
//bool ui_loader_has_ui(void)
//{
//    return _count_xmls_in(UI_SCREENS_DIR) > 0;
//}
//
//UiLoaderResult_t ui_loader_render(void)
//{
//    /* Reset diagnostics */
//    s_globals_register_err  = -99;
//    s_components_walked     = 0;
//    s_components_registered = 0;
//    s_screens_walked        = 0;
//    s_screens_registered    = 0;
//    s_create_screen_failed  = 0;
//
//    if (_count_xmls_in(UI_SCREENS_DIR) == 0)
//    {
//        return UI_LOADER_ERR_NO_FILES;
//    }
//
//    /* ---- Step 1: register globals.xml από Data Buffer ---- */
//    struct lfs_info gi;
//    if (lfs_stat(&lfs, UI_GLOBALS_LFS_PATH, &gi) == LFS_ERR_OK)
//    {
//        s_globals_register_err = _safe_register_from_file("globals", UI_GLOBALS_LFS_PATH);
//
//        if (s_globals_register_err != LV_RESULT_OK)
//        {
//            return UI_LOADER_ERR_GLOBALS;
//        }
//    }
//
//    /* ---- Step 2: register components ---- */
//    _walk_and_register(UI_COMPONENTS_DIR,
//                       &s_components_walked,
//                       &s_components_registered);
//
//    /* ---- Step 3: register screens ---- */
//    _walk_and_register(UI_SCREENS_DIR,
//                       &s_screens_walked,
//                       &s_screens_registered);
//
//    if (s_screens_walked > 0 && s_screens_registered == 0)
//    {
//        return UI_LOADER_ERR_LOAD;
//    }
//
//    /* ---- Step 4: Instantiate και load screen βάσει του Manual ---- */
//    lv_obj_t *scr = lv_xml_create_screen(STARTUP_SCREEN_NAME);
//    if (scr == NULL)
//    {
//        s_create_screen_failed = 1;
//        return UI_LOADER_ERR_CREATE_SCREEN;
//    }
//
//    lv_screen_load(scr);
//    return UI_LOADER_OK;
//}
//
//void ui_loader_show_placeholder(void)
//{
//    lv_obj_t *scr = lv_obj_create(NULL);
//    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1E3A8A), LV_PART_MAIN);
//
//    lv_obj_t *label = lv_label_create(scr);
//    lv_label_set_text(label, "Waiting for UI files...\nUpload via USB");
//    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
//    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
//    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
//
//    lv_screen_load(scr);
//}
//
//void ui_loader_show_error(UiLoaderResult_t err)
//{
//    lv_obj_t *scr = lv_obj_create(NULL);
//    lv_obj_set_style_bg_color(scr, lv_color_hex(0x8B0000), LV_PART_MAIN);
//
//    const char *err_name;
//    switch (err)
//    {
//        case UI_LOADER_ERR_NO_FILES:      err_name = "NO_FILES";      break;
//        case UI_LOADER_ERR_FS_OPEN:       err_name = "FS_OPEN";       break;
//        case UI_LOADER_ERR_GLOBALS:       err_name = "GLOBALS";       break;
//        case UI_LOADER_ERR_LOAD:          err_name = "LOAD_FAILED";   break;
//        case UI_LOADER_ERR_CREATE_SCREEN: err_name = "CREATE_FAILED"; break;
//        default:                          err_name = "UNKNOWN";       break;
//    }
//
//    static char buf[320];
//    snprintf(buf, sizeof(buf),
//             "UI LOAD ERROR\n"
//             "step:        %s\n"
//             "globals:     reg=%d\n"
//             "components:  %d/%d registered\n"
//             "screens:     %d/%d registered\n"
//             "create_main: %s",
//             err_name,
//             s_globals_register_err,
//             s_components_registered, s_components_walked,
//             s_screens_registered,    s_screens_walked,
//             s_create_screen_failed ? "FAIL" : "ok");
//
//    lv_obj_t *label = lv_label_create(scr);
//    lv_label_set_text(label, buf);
//    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
//    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
//    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
//
//    lv_screen_load(scr);
//}
//
///* =========================================================================
// * Private implementations
// * =========================================================================*/
//
//static bool _is_xml(const char *name)
//{
//    size_t n = strlen(name);
//    return (n >= 5) && (strcmp(&name[n - 4], ".xml") == 0);
//}
//
//static int _count_xmls_in(const char *dir_path)
//{
//    lfs_dir_t       dir;
//    struct lfs_info info;
//    int             count = 0;
//
//    if (lfs_dir_open(&lfs, &dir, dir_path) < 0)
//    {
//        return 0;
//    }
//
//    while (lfs_dir_read(&lfs, &dir, &info) > 0)
//    {
//        if (info.name[0] == '.')        continue;
//        if (info.type != LFS_TYPE_REG)  continue;
//        if (!_is_xml(info.name))        continue;
//        count++;
//    }
//
//    lfs_dir_close(&lfs, &dir);
//    return count;
//}
//
///**
// * @brief Ασφαλής ανάγνωση αρχείου στη RAM και εγγραφή μέσω της επίσημης
// * συνάρτησης lv_xml_register_component_from_data
// */
//static lv_result_t _safe_register_from_file(const char *component_name, const char *lfs_path)
//{
//    struct lfs_info info;
//    if (lfs_stat(&lfs, lfs_path, &info) != LFS_ERR_OK) {
//        return LV_RESULT_INVALID;
//    }
//
//    lfs_file_t f;
//    if (lfs_file_open(&lfs, &f, lfs_path, LFS_O_RDONLY) != LFS_ERR_OK) {
//        return LV_RESULT_INVALID;
//    }
//
//    char *xml_buf = malloc(info.size + 1);
//    if (xml_buf == NULL) {
//        lfs_file_close(&lfs, &f);
//        return LV_RESULT_INVALID;
//    }
//
//    lfs_file_read(&lfs, &f, xml_buf, info.size);
//    xml_buf[info.size] = '\0';
//
//    /* Κλείσιμο αρχείου αμέσως - Η LittleFS απελευθερώνεται! */
//    lfs_file_close(&lfs, &f);
//
//    /* Χρήση της σωστής συνάρτησης από το manual */
//    lv_result_t res = lv_xml_register_component_from_data(component_name, xml_buf);
//
//    free(xml_buf);
//    return res;
//}
//
//static void _walk_and_register(const char *dir_path,
//                                int        *out_walked,
//                                int        *out_registered)
//{
//    lfs_dir_t       dir;
//    struct lfs_info info;
//    int             walked     = 0;
//    int             registered = 0;
//
//    if (lfs_dir_open(&lfs, &dir, dir_path) < 0)
//    {
//        *out_walked     = 0;
//        *out_registered = 0;
//        return;
//    }
//
//    while (lfs_dir_read(&lfs, &dir, &info) > 0)
//    {
//        if (info.name[0] == '.')        continue;
//        if (info.type != LFS_TYPE_REG)  continue;
//        if (!_is_xml(info.name))        continue;
//
//        walked++;
//
//        char full_lfs_path[LFS_NAME_MAX + 64];
//        int written = snprintf(full_lfs_path, sizeof(full_lfs_path),
//                               "%s/%s", dir_path, info.name);
//
//        if (written < 0 || (size_t)written >= sizeof(full_lfs_path))
//        {
//            continue;
//        }
//
//        /* Απομονώνουμε το όνομα του αρχείου χωρίς το ".xml" για να το δώσουμε ως component name */
//        char comp_name[LFS_NAME_MAX];
//        size_t len = strlen(info.name);
//        strncpy(comp_name, info.name, len - 4);
//        comp_name[len - 4] = '\0';
//
//        if (_safe_register_from_file(comp_name, full_lfs_path) == LV_RESULT_OK)
//        {
//            registered++;
//        }
//    }
//
//    lfs_dir_close(&lfs, &dir);
//
//    *out_walked     = walked;
//    *out_registered = registered;
//}


/**
 ******************************************************************************
 * @file    ui_loader.c
 * @brief   Generic LVGL XML UI loader (Official 2026 Manual Compliant)
 ******************************************************************************
 */

#include "ui_loader.h"
#include "lfs.h"
#include "lfs_port.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Conventions
 * =========================================================================*/

#define STARTUP_SCREEN_NAME     "main_screen"

#define UI_GLOBALS_LFS_PATH     "ui/globals.xml"
#define UI_COMPONENTS_DIR       "ui/components"
#define UI_SCREENS_DIR          "ui/screens"

/* =========================================================================
 * Diagnostic state
 * =========================================================================*/

static int s_globals_register_err   = -99;
static int s_components_walked      = 0;
static int s_components_registered  = 0;
static int s_screens_walked         = 0;
static int s_screens_registered     = 0;
static int s_create_screen_failed   = 0;

/* =========================================================================
 * Private helpers
 * =========================================================================*/

static bool _is_xml          (const char *name);
static int  _count_xmls_in   (const char *dir_path);
static void _walk_and_register(const char *dir_path,
                                int        *out_walked,
                                int        *out_registered);
static lv_result_t _safe_register_from_file(const char *component_name, const char *lfs_path);

/* =========================================================================
 * Public API
 * =========================================================================*/

bool ui_loader_has_ui(void)
{
    return _count_xmls_in(UI_SCREENS_DIR) > 0;
}

UiLoaderResult_t ui_loader_render(void)
{
    /* Reset diagnostics */
    s_globals_register_err  = -99;
    s_components_walked     = 0;
    s_components_registered = 0;
    s_screens_walked        = 0;
    s_screens_registered    = 0;
    s_create_screen_failed  = 0;

    if (_count_xmls_in(UI_SCREENS_DIR) == 0)
    {
        return UI_LOADER_ERR_NO_FILES;
    }

    /* ---- Step 1: register globals.xml ---- */
    struct lfs_info gi;
    if (lfs_stat(&lfs, UI_GLOBALS_LFS_PATH, &gi) == LFS_ERR_OK)
    {
        /* IMPORTANT: globals.xml must be registered via
         * lv_xml_register_component_from_file(), NOT
         * lv_xml_register_component_from_data(). Per the official LVGL
         * 9.5 docs (docs.lvgl.io/9.5/xml/assets/images.html):
         *   "When registering globals.xml with
         *    lv_xml_register_component_from_file('A:path/to/globals.xml'),
         *    names are automatically mapped to the path... Fonts and
         *    Images are registered automatically when globals.xml is
         *    registered."
         * The previous approach here read the file into a RAM buffer and
         * called the _from_data variant instead — that parses the XML
         * fine (colors/consts show up), but does not appear to trigger
         * the automatic image/font name registration step, which is
         * exactly the symptom observed: <lv_image src="arrow"/> resolves
         * to nothing even though globals.xml correctly lists
         * <file name="arrow" src_path="A:ui/images/arrow.png"/>. */
        char lfs_full_path[64];
        snprintf(lfs_full_path, sizeof(lfs_full_path), "A:%s", UI_GLOBALS_LFS_PATH);

        s_globals_register_err = lv_xml_register_component_from_file(lfs_full_path);

        if (s_globals_register_err != LV_RESULT_OK)
        {
            return UI_LOADER_ERR_GLOBALS;
        }
    }

    /* ---- Step 2: register components ---- */
    _walk_and_register(UI_COMPONENTS_DIR,
                       &s_components_walked,
                       &s_components_registered);

    /* ---- Step 3: register screens ---- */
    _walk_and_register(UI_SCREENS_DIR,
                       &s_screens_walked,
                       &s_screens_registered);

    if (s_screens_walked > 0 && s_screens_registered == 0)
    {
        return UI_LOADER_ERR_LOAD;
    }

    /* ---- Step 4: Instantiate και load screen βάσει του Manual ---- */
    lv_obj_t *scr = lv_xml_create_screen(STARTUP_SCREEN_NAME);
    if (scr == NULL)
    {
        s_create_screen_failed = 1;
        return UI_LOADER_ERR_CREATE_SCREEN;
    }

    lv_screen_load(scr);
    return UI_LOADER_OK;
}

void ui_loader_show_placeholder(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1E3A8A), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Waiting for UI files...\nUpload via USB");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_screen_load(scr);
}

void ui_loader_show_error(UiLoaderResult_t err)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x8B0000), LV_PART_MAIN);

    const char *err_name;
    switch (err)
    {
        case UI_LOADER_ERR_NO_FILES:      err_name = "NO_FILES";      break;
        case UI_LOADER_ERR_FS_OPEN:       err_name = "FS_OPEN";       break;
        case UI_LOADER_ERR_GLOBALS:       err_name = "GLOBALS";       break;
        case UI_LOADER_ERR_LOAD:          err_name = "LOAD_FAILED";   break;
        case UI_LOADER_ERR_CREATE_SCREEN: err_name = "CREATE_FAILED"; break;
        default:                          err_name = "UNKNOWN";       break;
    }

    static char buf[320];
    snprintf(buf, sizeof(buf),
             "UI LOAD ERROR\n"
             "step:        %s\n"
             "globals:     reg=%d\n"
             "components:  %d/%d registered\n"
             "screens:     %d/%d registered\n"
             "create_main: %s",
             err_name,
             s_globals_register_err,
             s_components_registered, s_components_walked,
             s_screens_registered,    s_screens_walked,
             s_create_screen_failed ? "FAIL" : "ok");

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, buf);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_screen_load(scr);
}

/* =========================================================================
 * Private implementations
 * =========================================================================*/

static bool _is_xml(const char *name)
{
    size_t n = strlen(name);
    return (n >= 5) && (strcmp(&name[n - 4], ".xml") == 0);
}

static int _count_xmls_in(const char *dir_path)
{
    lfs_dir_t       dir;
    struct lfs_info info;
    int             count = 0;

    if (lfs_dir_open(&lfs, &dir, dir_path) < 0)
    {
        return 0;
    }

    while (lfs_dir_read(&lfs, &dir, &info) > 0)
    {
        if (info.name[0] == '.')        continue;
        if (info.type != LFS_TYPE_REG)  continue;
        if (!_is_xml(info.name))        continue;
        count++;
    }

    lfs_dir_close(&lfs, &dir);
    return count;
}

/**
 * @brief Ασφαλής ανάγνωση αρχείου στη RAM και εγγραφή μέσω της επίσημης
 * συνάρτησης lv_xml_register_component_from_data.
 *
 * Χρησιμοποιείται ΜΟΝΟ για components/screens πλέον — ΟΧΙ για globals.xml,
 * βλέπε το σχόλιο στο ui_loader_render() Step 1 για το γιατί.
 */
static lv_result_t _safe_register_from_file(const char *component_name, const char *lfs_path)
{
    struct lfs_info info;
    if (lfs_stat(&lfs, lfs_path, &info) != LFS_ERR_OK) {
        return LV_RESULT_INVALID;
    }

    lfs_file_t f;
    if (lfs_file_open(&lfs, &f, lfs_path, LFS_O_RDONLY) != LFS_ERR_OK) {
        return LV_RESULT_INVALID;
    }

    char *xml_buf = malloc(info.size + 1);
    if (xml_buf == NULL) {
        lfs_file_close(&lfs, &f);
        return LV_RESULT_INVALID;
    }

    lfs_file_read(&lfs, &f, xml_buf, info.size);
    xml_buf[info.size] = '\0';

    /* Κλείσιμο αρχείου αμέσως - Η LittleFS απελευθερώνεται! */
    lfs_file_close(&lfs, &f);

    /* Χρήση της σωστής συνάρτησης από το manual */
    lv_result_t res = lv_xml_register_component_from_data(component_name, xml_buf);

    free(xml_buf);
    return res;
}

static void _walk_and_register(const char *dir_path,
                                int        *out_walked,
                                int        *out_registered)
{
    lfs_dir_t       dir;
    struct lfs_info info;
    int             walked     = 0;
    int             registered = 0;

    if (lfs_dir_open(&lfs, &dir, dir_path) < 0)
    {
        *out_walked     = 0;
        *out_registered = 0;
        return;
    }

    while (lfs_dir_read(&lfs, &dir, &info) > 0)
    {
        if (info.name[0] == '.')        continue;
        if (info.type != LFS_TYPE_REG)  continue;
        if (!_is_xml(info.name))        continue;

        walked++;

        char full_lfs_path[LFS_NAME_MAX + 64];
        int written = snprintf(full_lfs_path, sizeof(full_lfs_path),
                               "%s/%s", dir_path, info.name);

        if (written < 0 || (size_t)written >= sizeof(full_lfs_path))
        {
            continue;
        }

        /* Απομονώνουμε το όνομα του αρχείου χωρίς το ".xml" για να το δώσουμε ως component name */
        char comp_name[LFS_NAME_MAX];
        size_t len = strlen(info.name);
        strncpy(comp_name, info.name, len - 4);
        comp_name[len - 4] = '\0';

        if (_safe_register_from_file(comp_name, full_lfs_path) == LV_RESULT_OK)
        {
            registered++;
        }
    }

    lfs_dir_close(&lfs, &dir);

    *out_walked     = walked;
    *out_registered = registered;
}
