///**
// ******************************************************************************
// * @file    usb_at_handler.c
// * @brief   AT command handler over USB CDC for file upload to LittleFS.
// *
// *  Supported commands:
// *      AT+UPLOAD="<path>",<size>   Upload a single file to flash
// *      AT+RESET                    Software reset (used after batch upload)
// *
// *  Upload protocol:
// *      Host:    AT+UPLOAD="ui/screens/main.xml",1234\r\n
// *      Device:  +READY\r\n          (or +ERROR,<reason>\r\n)
// *      Host:    <1234 raw bytes>
// *      Host:    <4 bytes CRC32 little-endian>
// *      Device:  +OK\r\n             (or +ERROR,<reason>\r\n)
// *
// *  CRC32 = zlib.crc32(filename + file_bytes), polynomial 0xEDB88320.
// *
// *  ARCHITECTURE
// *  ============
// *      USB ISR (CDC_Receive) -> stream buffer -> AT task -> CDC_Transmit
// *
// *      The AT task is the single owner of CDC_Transmit, eliminating
// *      ISR-vs-task races on the TX endpoint.
// ******************************************************************************
// */
//
//#include "usb_at_handler.h"
//
//#include "FreeRTOS.h"
//#include "task.h"
//#include "stream_buffer.h"
//#include "cmsis_os2.h"
//
//#include "usbd_cdc_if.h"
//#include "lfs.h"
//#include "lfs_port.h"           /* extern lfs */
//
//#include "stm32u5xx_hal.h"      /* NVIC_SystemReset */
//
//#include <string.h>
//#include <stdlib.h>
//#include <stdio.h>
//#include <stdbool.h>
//
///* =========================================================================
// * Configuration
// * =========================================================================*/
//
//#define AT_RX_STREAM_SIZE       1024U
//#define AT_LINE_MAX             256U
//#define AT_FILENAME_MAX         128U
//#define AT_FILE_MAX_SIZE        (4U * 1024U * 1024U)
//
//#define AT_BYTE_TIMEOUT_MS      5000U
//
//#define AT_TX_RETRY_MAX         20U
//#define AT_TX_RETRY_DELAY_MS    5U
//
///* =========================================================================
// * State machine
// * =========================================================================*/
//
//typedef enum {
//    AT_STATE_IDLE = 0,
//    AT_STATE_RECEIVING_DATA,
//    AT_STATE_RECEIVING_CRC
//} at_state_t;
//
///* =========================================================================
// * Module state
// * =========================================================================*/
//
//static StreamBufferHandle_t   s_rx_stream;
//static StaticStreamBuffer_t   s_rx_stream_struct;
//static uint8_t                s_rx_stream_storage[AT_RX_STREAM_SIZE + 1];
//
//static osThreadId_t           s_at_task_handle;
//static const osThreadAttr_t   s_at_task_attr = {
//    .name       = "usbAtTask",
//    .priority   = (osPriority_t)osPriorityNormal,
//    .stack_size = 4096
//};
//
//static struct {
//    char        filename[AT_FILENAME_MAX];
//    uint32_t    expected_size;
//    uint32_t    bytes_received;
//    uint32_t    crc;
//    uint8_t     crc_bytes[4];
//    uint8_t     crc_idx;
//    lfs_file_t  file;
//    bool        file_open;
//} s_upload;
//
//static at_state_t s_state = AT_STATE_IDLE;
//static char       s_line[AT_LINE_MAX];
//static uint16_t   s_line_len = 0U;
//static char       s_swallow_partner = '\0';
//
///* =========================================================================
// * Forward declarations
// * =========================================================================*/
//
//static void     _at_task(void *argument);
//static void     _send_response(const char *resp);
//
//static void     _state_idle_handle_line(const char *line);
//static void     _state_receiving_data_handle_byte(uint8_t byte);
//static void     _state_receiving_crc_handle_byte(uint8_t byte);
//
//static int      _parse_upload_cmd(const char *line, char *out_name,
//                                  uint32_t *out_size);
//static int      _ensure_parent_dirs(const char *path);
//static void     _abort_upload(const char *error_reason);
//
//static uint32_t _crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len);
//
///* =========================================================================
// * Public API
// * =========================================================================*/
//
//void usb_at_init(void)
//{
//    s_rx_stream = xStreamBufferCreateStatic(AT_RX_STREAM_SIZE, 1U,
//                                            s_rx_stream_storage,
//                                            &s_rx_stream_struct);
//
//    s_at_task_handle = osThreadNew(_at_task, NULL, &s_at_task_attr);
//}
//
//void usb_at_feed_byte_from_isr(uint8_t byte)
//{
//    BaseType_t higher_priority_task_woken = pdFALSE;
//    (void)xStreamBufferSendFromISR(s_rx_stream, &byte, 1U,
//                                   &higher_priority_task_woken);
//    portYIELD_FROM_ISR(higher_priority_task_woken);
//}
//
///* =========================================================================
// * Main task
// * =========================================================================*/
//
//static void _at_task(void *argument)
//{
//    (void)argument;
//
//    uint8_t byte;
//    TickType_t timeout_ticks;
//
//    for (;;)
//    {
//        timeout_ticks = (s_state == AT_STATE_IDLE)
//                          ? portMAX_DELAY
//                          : pdMS_TO_TICKS(AT_BYTE_TIMEOUT_MS);
//
//        size_t got = xStreamBufferReceive(s_rx_stream, &byte, 1U, timeout_ticks);
//
//        if (got == 0U)
//        {
//            if (s_state != AT_STATE_IDLE)
//            {
//                _abort_upload("TIMEOUT");
//            }
//            continue;
//        }
//
//        /* Swallow the partner CR/LF after a line terminator that triggered
//         * a state transition (otherwise it would leak into the next state). */
//        if (s_swallow_partner != '\0')
//        {
//            char partner = s_swallow_partner;
//            s_swallow_partner = '\0';
//            if (byte == (uint8_t)partner)
//            {
//                continue;
//            }
//        }
//
//        switch (s_state)
//        {
//            case AT_STATE_IDLE:
//                if (byte == '\r' || byte == '\n')
//                {
//                    if (s_line_len > 0U)
//                    {
//                        char terminator = (char)byte;
//                        s_line[s_line_len] = '\0';
//                        _state_idle_handle_line(s_line);
//                        s_line_len = 0U;
//
//                        if (s_state != AT_STATE_IDLE)
//                        {
//                            s_swallow_partner =
//                                (terminator == '\r') ? '\n' : '\r';
//                        }
//                    }
//                }
//                else if (s_line_len < (AT_LINE_MAX - 1U))
//                {
//                    s_line[s_line_len++] = (char)byte;
//                }
//                else
//                {
//                    s_line_len = 0U;
//                    _send_response("+ERROR,LINE_OVERFLOW\r\n");
//                }
//                break;
//
//            case AT_STATE_RECEIVING_DATA:
//                _state_receiving_data_handle_byte(byte);
//                break;
//
//            case AT_STATE_RECEIVING_CRC:
//                _state_receiving_crc_handle_byte(byte);
//                break;
//        }
//    }
//}
//
///* =========================================================================
// * IDLE state — parse and dispatch AT commands
// * =========================================================================*/
//
//static void _state_idle_handle_line(const char *line)
//{
//    /* ---------- AT+RESET ---------- *
//     * Acknowledge first, give the USB IN endpoint time to drain the +OK,
//     * then reset the MCU. After reset, ui_loader_has_ui() will detect the
//     * freshly uploaded files and render the new UI. */
//    if (strcmp(line, "AT+RESET") == 0)
//    {
//        _send_response("+OK\r\n");
//        osDelay(200);                /* let +OK reach the host */
//        NVIC_SystemReset();
//        /* never returns */
//    }
//
//    /* ---------- AT+HWINFO? (ΝΕΑ ΕΝΤΟΛΗ) ---------- */
//	if (strcmp(line, "AT+HWINFO?") == 0)
//	{
//		char info_resp[128];
//
//		// Συνθέτουμε την απάντηση σε μορφή: +HWINFO:<MODEL>,<WIDTH>x<HEIGHT>,<FW_VERSION>
//		int len = snprintf(info_resp, sizeof(info_resp), "+HWINFO:\"%s\",%dx%d,\"%s\"\r\n", "Riverdi-STM32U599", 800, 480, "1.0.0");
//
//		if (len > 0)
//		{
//			_send_response(info_resp);
//			_send_response("+OK\r\n");
//		}
//		else
//		{
//			_send_response("+ERROR,BUFF\r\n");
//		}
//		return;
//	}
//
//    /* ---------- AT+UPLOAD="path",size ---------- */
//    static const char prefix[] = "AT+UPLOAD=";
//
//    if (strncmp(line, prefix, sizeof(prefix) - 1U) != 0)
//    {
//        _send_response("+ERROR,UNKNOWN_CMD\r\n");
//        return;
//    }
//
//    char     filename[AT_FILENAME_MAX];
//    uint32_t size;
//
//    if (_parse_upload_cmd(line, filename, &size) != 0)
//    {
//        _send_response("+ERROR,SYNTAX\r\n");
//        return;
//    }
//
//    if (size == 0U || size > AT_FILE_MAX_SIZE)
//    {
//        _send_response("+ERROR,SIZE\r\n");
//        return;
//    }
//
//    if (filename[0] == '\0')
//    {
//        _send_response("+ERROR,FILENAME\r\n");
//        return;
//    }
//
//    if (_ensure_parent_dirs(filename) != 0)
//    {
//        _send_response("+ERROR,MKDIR\r\n");
//        return;
//    }
//
//    int err = lfs_file_open(&lfs, &s_upload.file, filename,
//                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
//    if (err != LFS_ERR_OK)
//    {
//        _send_response("+ERROR,OPEN\r\n");
//        return;
//    }
//
//    strncpy(s_upload.filename, filename, AT_FILENAME_MAX - 1);
//    s_upload.filename[AT_FILENAME_MAX - 1] = '\0';
//    s_upload.expected_size  = size;
//    s_upload.bytes_received = 0U;
//    s_upload.crc_idx        = 0U;
//    s_upload.file_open      = true;
//
//    /* Seed CRC32 with filename bytes (no quotes) */
//    s_upload.crc = 0xFFFFFFFFU;
//    s_upload.crc = _crc32_update(s_upload.crc,
//                                 (const uint8_t *)s_upload.filename,
//                                 (uint32_t)strlen(s_upload.filename));
//
//    _send_response("+READY\r\n");
//    s_state = AT_STATE_RECEIVING_DATA;
//}
//
///* =========================================================================
// * RECEIVING_DATA: write bytes to LFS, accumulate CRC32
// * =========================================================================*/
//
//static void _state_receiving_data_handle_byte(uint8_t byte)
//{
//    lfs_ssize_t w = lfs_file_write(&lfs, &s_upload.file, &byte, 1U);
//    if (w != 1)
//    {
//        _abort_upload("WRITE");
//        return;
//    }
//
//    s_upload.crc = _crc32_update(s_upload.crc, &byte, 1U);
//    s_upload.bytes_received++;
//
//    if (s_upload.bytes_received >= s_upload.expected_size)
//    {
//        s_state = AT_STATE_RECEIVING_CRC;
//    }
//}
//
///* =========================================================================
// * RECEIVING_CRC: collect 4 bytes (little-endian), verify, finalise
// * =========================================================================*/
//
//static void _state_receiving_crc_handle_byte(uint8_t byte)
//{
//    s_upload.crc_bytes[s_upload.crc_idx++] = byte;
//
//    if (s_upload.crc_idx < 4U)
//    {
//        return;
//    }
//
//    uint32_t expected_crc =
//          ((uint32_t)s_upload.crc_bytes[0])
//        | ((uint32_t)s_upload.crc_bytes[1] << 8)
//        | ((uint32_t)s_upload.crc_bytes[2] << 16)
//        | ((uint32_t)s_upload.crc_bytes[3] << 24);
//
//    uint32_t computed_crc = s_upload.crc ^ 0xFFFFFFFFU;
//
//    int close_err = lfs_file_close(&lfs, &s_upload.file);
//    s_upload.file_open = false;
//
//    if (computed_crc != expected_crc)
//    {
//        char resp[64];
//        int len = snprintf(resp, sizeof(resp),
//                           "+ERROR,CRC,%08lX,%08lX\r\n",
//                           (unsigned long)computed_crc,
//                           (unsigned long)expected_crc);
//        if (len > 0) _send_response(resp);
//    }
//    else if (close_err != LFS_ERR_OK)
//    {
//        _send_response("+ERROR,CLOSE\r\n");
//    }
//    else
//    {
//        _send_response("+OK\r\n");
//    }
//
//    s_state = AT_STATE_IDLE;
//    s_line_len = 0U;
//}
//
///* =========================================================================
// * Helpers
// * =========================================================================*/
//
//static int _parse_upload_cmd(const char *line, char *out_name, uint32_t *out_size)
//{
//    const char *p = line + (sizeof("AT+UPLOAD=") - 1U);
//
//    if (*p != '"') return -1;
//    p++;
//
//    size_t i = 0;
//    while (*p != '\0' && *p != '"' && i < (AT_FILENAME_MAX - 1U))
//    {
//        out_name[i++] = *p++;
//    }
//    if (*p != '"') return -1;
//    out_name[i] = '\0';
//    p++;
//
//    if (*p != ',') return -1;
//    p++;
//
//    char *endp;
//    unsigned long val = strtoul(p, &endp, 10);
//    if (endp == p) return -1;
//    if (val > UINT32_MAX) return -1;
//
//    *out_size = (uint32_t)val;
//    return 0;
//}
//
//static int _ensure_parent_dirs(const char *path)
//{
//    char buf[AT_FILENAME_MAX];
//    strncpy(buf, path, sizeof(buf) - 1);
//    buf[sizeof(buf) - 1] = '\0';
//
//    for (char *p = buf + 1; *p != '\0'; p++)
//    {
//        if (*p == '/')
//        {
//            *p = '\0';
//            int err = lfs_mkdir(&lfs, buf);
//            if (err != LFS_ERR_OK && err != LFS_ERR_EXIST)
//            {
//                return -1;
//            }
//            *p = '/';
//        }
//    }
//    return 0;
//}
//
//static void _abort_upload(const char *error_reason)
//{
//    if (s_upload.file_open)
//    {
//        (void)lfs_file_close(&lfs, &s_upload.file);
//        s_upload.file_open = false;
//    }
//
//    char resp[64];
//    int len = snprintf(resp, sizeof(resp), "+ERROR,%s\r\n", error_reason);
//    if (len > 0)
//    {
//        _send_response(resp);
//    }
//
//    s_state    = AT_STATE_IDLE;
//    s_line_len = 0U;
//}
//
//static void _send_response(const char *resp)
//{
//    uint16_t len = (uint16_t)strlen(resp);
//
//    for (uint32_t attempt = 0; attempt < AT_TX_RETRY_MAX; attempt++)
//    {
//        if (CDC_Transmit((uint8_t *)resp, len) == USBD_OK)
//        {
//            return;
//        }
//        osDelay(AT_TX_RETRY_DELAY_MS);
//    }
//}
//
///* =========================================================================
// * Software CRC32 (zlib / PKZIP standard)
// *   Polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
// *   Compatible with Python's zlib.crc32() and binascii.crc32().
// * =========================================================================*/
//
//static uint32_t s_crc_table[256];
//static bool     s_crc_table_ready = false;
//
//static void _crc32_init_table(void)
//{
//    for (uint32_t i = 0; i < 256; i++)
//    {
//        uint32_t c = i;
//        for (int j = 0; j < 8; j++)
//        {
//            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
//        }
//        s_crc_table[i] = c;
//    }
//    s_crc_table_ready = true;
//}
//
//static uint32_t _crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
//{
//    if (!s_crc_table_ready) _crc32_init_table();
//
//    for (uint32_t i = 0; i < len; i++)
//    {
//        crc = s_crc_table[(crc ^ buf[i]) & 0xFFU] ^ (crc >> 8);
//    }
//    return crc;
//}


/**
 ******************************************************************************
 * @file    usb_at_handler.c
 * @brief   AT command handler over USB CDC for file upload to LittleFS.
 *
 *  Supported commands:
 *      AT+WIPEUI                   Clear stale ui/screens, ui/components,
 *                                   ui/images, ui/fonts before a new deploy
 *      AT+UPLOAD="<path>",<size>   Upload a single file to flash
 *      AT+RESET                    Software reset (used after batch upload)
 *      AT+HWINFO?                  Report hardware model / resolution / fw
 *
 *  Upload protocol:
 *      Host:    AT+UPLOAD="ui/screens/main.xml",1234\r\n
 *      Device:  +READY\r\n          (or +ERROR,<reason>\r\n)
 *      Host:    <1234 raw bytes>
 *      Host:    <4 bytes CRC32 little-endian>
 *      Device:  +OK\r\n             (or +ERROR,<reason>\r\n)
 *
 *  CRC32 = zlib.crc32(filename + file_bytes), polynomial 0xEDB88320.
 *
 *  ARCHITECTURE
 *  ============
 *      USB ISR (CDC_Receive) -> stream buffer -> AT task -> CDC_Transmit
 *
 *      The AT task is the single owner of CDC_Transmit, eliminating
 *      ISR-vs-task races on the TX endpoint.
 ******************************************************************************
 */

#include "usb_at_handler.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "cmsis_os2.h"

#include "usbd_cdc_if.h"
#include "lfs.h"
#include "lfs_port.h"           /* extern lfs */

#include "stm32u5xx_hal.h"      /* NVIC_SystemReset */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

/* =========================================================================
 * Configuration
 * =========================================================================*/

#define AT_RX_STREAM_SIZE       1024U
#define AT_LINE_MAX             256U
#define AT_FILENAME_MAX         128U
#define AT_FILE_MAX_SIZE        (4U * 1024U * 1024U)

#define AT_BYTE_TIMEOUT_MS      5000U

#define AT_TX_RETRY_MAX         20U
#define AT_TX_RETRY_DELAY_MS    5U

/* =========================================================================
 * State machine
 * =========================================================================*/

typedef enum {
    AT_STATE_IDLE = 0,
    AT_STATE_RECEIVING_DATA,
    AT_STATE_RECEIVING_CRC
} at_state_t;

/* =========================================================================
 * Module state
 * =========================================================================*/

static StreamBufferHandle_t   s_rx_stream;
static StaticStreamBuffer_t   s_rx_stream_struct;
static uint8_t                s_rx_stream_storage[AT_RX_STREAM_SIZE + 1];

static osThreadId_t           s_at_task_handle;
static const osThreadAttr_t   s_at_task_attr = {
    .name       = "usbAtTask",
    .priority   = (osPriority_t)osPriorityNormal,
    .stack_size = 4096
};

static struct {
    char        filename[AT_FILENAME_MAX];
    uint32_t    expected_size;
    uint32_t    bytes_received;
    uint32_t    crc;
    uint8_t     crc_bytes[4];
    uint8_t     crc_idx;
    lfs_file_t  file;
    bool        file_open;
} s_upload;

static at_state_t s_state = AT_STATE_IDLE;
static char       s_line[AT_LINE_MAX];
static uint16_t   s_line_len = 0U;
static char       s_swallow_partner = '\0';

/* =========================================================================
 * Forward declarations
 * =========================================================================*/

static void     _at_task(void *argument);
static void     _send_response(const char *resp);

static void     _state_idle_handle_line(const char *line);
static void     _state_receiving_data_handle_byte(uint8_t byte);
static void     _state_receiving_crc_handle_byte(uint8_t byte);

static int      _parse_upload_cmd(const char *line, char *out_name,
                                  uint32_t *out_size);
static int      _ensure_parent_dirs(const char *path);
static void     _abort_upload(const char *error_reason);
static void     _wipe_dir_files(const char *dir_path);

static uint32_t _crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len);

/* =========================================================================
 * Public API
 * =========================================================================*/

void usb_at_init(void)
{
    s_rx_stream = xStreamBufferCreateStatic(AT_RX_STREAM_SIZE, 1U,
                                            s_rx_stream_storage,
                                            &s_rx_stream_struct);

    s_at_task_handle = osThreadNew(_at_task, NULL, &s_at_task_attr);
}

void usb_at_feed_byte_from_isr(uint8_t byte)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    (void)xStreamBufferSendFromISR(s_rx_stream, &byte, 1U,
                                   &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* =========================================================================
 * Main task
 * =========================================================================*/

static void _at_task(void *argument)
{
    (void)argument;

    uint8_t byte;
    TickType_t timeout_ticks;

    for (;;)
    {
        timeout_ticks = (s_state == AT_STATE_IDLE)
                          ? portMAX_DELAY
                          : pdMS_TO_TICKS(AT_BYTE_TIMEOUT_MS);

        size_t got = xStreamBufferReceive(s_rx_stream, &byte, 1U, timeout_ticks);

        if (got == 0U)
        {
            if (s_state != AT_STATE_IDLE)
            {
                _abort_upload("TIMEOUT");
            }
            continue;
        }

        /* Swallow the partner CR/LF after a line terminator that triggered
         * a state transition (otherwise it would leak into the next state). */
        if (s_swallow_partner != '\0')
        {
            char partner = s_swallow_partner;
            s_swallow_partner = '\0';
            if (byte == (uint8_t)partner)
            {
                continue;
            }
        }

        switch (s_state)
        {
            case AT_STATE_IDLE:
                if (byte == '\r' || byte == '\n')
                {
                    if (s_line_len > 0U)
                    {
                        char terminator = (char)byte;
                        s_line[s_line_len] = '\0';
                        _state_idle_handle_line(s_line);
                        s_line_len = 0U;

                        if (s_state != AT_STATE_IDLE)
                        {
                            s_swallow_partner =
                                (terminator == '\r') ? '\n' : '\r';
                        }
                    }
                }
                else if (s_line_len < (AT_LINE_MAX - 1U))
                {
                    s_line[s_line_len++] = (char)byte;
                }
                else
                {
                    s_line_len = 0U;
                    _send_response("+ERROR,LINE_OVERFLOW\r\n");
                }
                break;

            case AT_STATE_RECEIVING_DATA:
                _state_receiving_data_handle_byte(byte);
                break;

            case AT_STATE_RECEIVING_CRC:
                _state_receiving_crc_handle_byte(byte);
                break;
        }
    }
}

/* =========================================================================
 * IDLE state — parse and dispatch AT commands
 * =========================================================================*/

static void _state_idle_handle_line(const char *line)
{
    /* ---------- AT+RESET ---------- *
     * Acknowledge first, give the USB IN endpoint time to drain the +OK,
     * then reset the MCU. After reset, ui_loader_has_ui() will detect the
     * freshly uploaded files and render the new UI. */
    if (strcmp(line, "AT+RESET") == 0)
    {
        _send_response("+OK\r\n");
        osDelay(200);                /* let +OK reach the host */
        NVIC_SystemReset();
        /* never returns */
    }

    /* ---------- AT+WIPEUI ---------- *
     * Clears every file under ui/screens, ui/components, ui/images and
     * ui/fonts BEFORE a new deploy uploads its files. Without this, files
     * from a previous project/screen name (renamed or deleted screens,
     * orphaned images) linger on flash forever — AT+UPLOAD only truncates
     * the exact path it's given, it never removes anything else. The C#
     * side (UsbService.UploadProject) sends this once at the start of
     * every deploy, before any AT+UPLOAD calls. */
    if (strcmp(line, "AT+WIPEUI") == 0)
    {
        _wipe_dir_files("ui/screens");
        _wipe_dir_files("ui/components");
        _wipe_dir_files("ui/images");
        _wipe_dir_files("ui/fonts");
        _send_response("+OK\r\n");
        return;
    }

    /* ---------- AT+HWINFO? (ΝΕΑ ΕΝΤΟΛΗ) ---------- */
	if (strcmp(line, "AT+HWINFO?") == 0)
	{
		char info_resp[128];

		// Συνθέτουμε την απάντηση σε μορφή: +HWINFO:<MODEL>,<WIDTH>x<HEIGHT>,<FW_VERSION>
		int len = snprintf(info_resp, sizeof(info_resp), "+HWINFO:\"%s\",%dx%d,\"%s\"\r\n", "Riverdi-STM32U599", 800, 480, "1.0.0");

		if (len > 0)
		{
			_send_response(info_resp);
			_send_response("+OK\r\n");
		}
		else
		{
			_send_response("+ERROR,BUFF\r\n");
		}
		return;
	}

    /* ---------- AT+UPLOAD="path",size ---------- */
    static const char prefix[] = "AT+UPLOAD=";

    if (strncmp(line, prefix, sizeof(prefix) - 1U) != 0)
    {
        _send_response("+ERROR,UNKNOWN_CMD\r\n");
        return;
    }

    char     filename[AT_FILENAME_MAX];
    uint32_t size;

    if (_parse_upload_cmd(line, filename, &size) != 0)
    {
        _send_response("+ERROR,SYNTAX\r\n");
        return;
    }

    if (size == 0U || size > AT_FILE_MAX_SIZE)
    {
        _send_response("+ERROR,SIZE\r\n");
        return;
    }

    if (filename[0] == '\0')
    {
        _send_response("+ERROR,FILENAME\r\n");
        return;
    }

    if (_ensure_parent_dirs(filename) != 0)
    {
        _send_response("+ERROR,MKDIR\r\n");
        return;
    }

    int err = lfs_file_open(&lfs, &s_upload.file, filename,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK)
    {
        _send_response("+ERROR,OPEN\r\n");
        return;
    }

    strncpy(s_upload.filename, filename, AT_FILENAME_MAX - 1);
    s_upload.filename[AT_FILENAME_MAX - 1] = '\0';
    s_upload.expected_size  = size;
    s_upload.bytes_received = 0U;
    s_upload.crc_idx        = 0U;
    s_upload.file_open      = true;

    /* Seed CRC32 with filename bytes (no quotes) */
    s_upload.crc = 0xFFFFFFFFU;
    s_upload.crc = _crc32_update(s_upload.crc,
                                 (const uint8_t *)s_upload.filename,
                                 (uint32_t)strlen(s_upload.filename));

    _send_response("+READY\r\n");
    s_state = AT_STATE_RECEIVING_DATA;
}

/* =========================================================================
 * RECEIVING_DATA: write bytes to LFS, accumulate CRC32
 * =========================================================================*/

static void _state_receiving_data_handle_byte(uint8_t byte)
{
    lfs_ssize_t w = lfs_file_write(&lfs, &s_upload.file, &byte, 1U);
    if (w != 1)
    {
        _abort_upload("WRITE");
        return;
    }

    s_upload.crc = _crc32_update(s_upload.crc, &byte, 1U);
    s_upload.bytes_received++;

    if (s_upload.bytes_received >= s_upload.expected_size)
    {
        s_state = AT_STATE_RECEIVING_CRC;
    }
}

/* =========================================================================
 * RECEIVING_CRC: collect 4 bytes (little-endian), verify, finalise
 * =========================================================================*/

static void _state_receiving_crc_handle_byte(uint8_t byte)
{
    s_upload.crc_bytes[s_upload.crc_idx++] = byte;

    if (s_upload.crc_idx < 4U)
    {
        return;
    }

    uint32_t expected_crc =
          ((uint32_t)s_upload.crc_bytes[0])
        | ((uint32_t)s_upload.crc_bytes[1] << 8)
        | ((uint32_t)s_upload.crc_bytes[2] << 16)
        | ((uint32_t)s_upload.crc_bytes[3] << 24);

    uint32_t computed_crc = s_upload.crc ^ 0xFFFFFFFFU;

    int close_err = lfs_file_close(&lfs, &s_upload.file);
    s_upload.file_open = false;

    if (computed_crc != expected_crc)
    {
        char resp[64];
        int len = snprintf(resp, sizeof(resp),
                           "+ERROR,CRC,%08lX,%08lX\r\n",
                           (unsigned long)computed_crc,
                           (unsigned long)expected_crc);
        if (len > 0) _send_response(resp);
    }
    else if (close_err != LFS_ERR_OK)
    {
        _send_response("+ERROR,CLOSE\r\n");
    }
    else
    {
        _send_response("+OK\r\n");
    }

    s_state = AT_STATE_IDLE;
    s_line_len = 0U;
}

/* =========================================================================
 * Helpers
 * =========================================================================*/

static int _parse_upload_cmd(const char *line, char *out_name, uint32_t *out_size)
{
    const char *p = line + (sizeof("AT+UPLOAD=") - 1U);

    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (*p != '\0' && *p != '"' && i < (AT_FILENAME_MAX - 1U))
    {
        out_name[i++] = *p++;
    }
    if (*p != '"') return -1;
    out_name[i] = '\0';
    p++;

    if (*p != ',') return -1;
    p++;

    char *endp;
    unsigned long val = strtoul(p, &endp, 10);
    if (endp == p) return -1;
    if (val > UINT32_MAX) return -1;

    *out_size = (uint32_t)val;
    return 0;
}

static int _ensure_parent_dirs(const char *path)
{
    char buf[AT_FILENAME_MAX];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char *p = buf + 1; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            int err = lfs_mkdir(&lfs, buf);
            if (err != LFS_ERR_OK && err != LFS_ERR_EXIST)
            {
                return -1;
            }
            *p = '/';
        }
    }
    return 0;
}

static void _abort_upload(const char *error_reason)
{
    if (s_upload.file_open)
    {
        (void)lfs_file_close(&lfs, &s_upload.file);
        s_upload.file_open = false;
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "+ERROR,%s\r\n", error_reason);
    if (len > 0)
    {
        _send_response(resp);
    }

    s_state    = AT_STATE_IDLE;
    s_line_len = 0U;
}

/**
 * @brief  Delete every regular file directly inside dir_path (non-recursive
 *         into further subdirectories — ui/screens, ui/images etc. are all
 *         flat directories by convention, so this is sufficient). The
 *         directory itself is left in place; only its file contents are
 *         removed, since ui_storage_init() expects these directories to
 *         already exist on every boot.
 */
static void _wipe_dir_files(const char *dir_path)
{
    lfs_dir_t       dir;
    struct lfs_info info;

    if (lfs_dir_open(&lfs, &dir, dir_path) < 0)
    {
        return;
    }

    while (lfs_dir_read(&lfs, &dir, &info) > 0)
    {
        if (info.name[0] == '.')       continue;
        if (info.type != LFS_TYPE_REG) continue;

        char full_path[LFS_NAME_MAX + 64];
        int written = snprintf(full_path, sizeof(full_path),
                               "%s/%s", dir_path, info.name);
        if (written < 0 || (size_t)written >= sizeof(full_path))
        {
            continue;
        }

        (void)lfs_remove(&lfs, full_path);
    }

    lfs_dir_close(&lfs, &dir);
}

static void _send_response(const char *resp)
{
    uint16_t len = (uint16_t)strlen(resp);

    for (uint32_t attempt = 0; attempt < AT_TX_RETRY_MAX; attempt++)
    {
        if (CDC_Transmit((uint8_t *)resp, len) == USBD_OK)
        {
            return;
        }
        osDelay(AT_TX_RETRY_DELAY_MS);
    }
}

/* =========================================================================
 * Software CRC32 (zlib / PKZIP standard)
 *   Polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 *   Compatible with Python's zlib.crc32() and binascii.crc32().
 * =========================================================================*/

static uint32_t s_crc_table[256];
static bool     s_crc_table_ready = false;

static void _crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
        {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_table_ready = true;
}

static uint32_t _crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    if (!s_crc_table_ready) _crc32_init_table();

    for (uint32_t i = 0; i < len; i++)
    {
        crc = s_crc_table[(crc ^ buf[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}
