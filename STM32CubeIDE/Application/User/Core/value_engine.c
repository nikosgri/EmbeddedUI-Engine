/**
 ******************************************************************************
 * @file    value_engine.c
 * @brief   Fills the UI's Values (LVGL subjects) from the hardware.
 *
 *  Two halves, so the hardware side never touches LVGL:
 *
 *    value_engine_load()   main(), right after ui_loader_render(), before the
 *                          scheduler (same context as the UI loader):
 *                          reads ui/values.json, finds the subjects, sets the
 *                          pins up, creates an LVGL timer.
 *
 *    hardware task         (value_engine_start, from MX_FREERTOS_Init)
 *                          calibrates the ADC, then samples the pins and
 *                          computes each value. No LVGL calls at all.
 *
 *    LVGL timer            runs INSIDE the LVGL task (lv_timer_handler), so
 *                          it may set subjects without any locking: display
 *                          filter, int subject, text subject.
 *
 *  Per Value, the same chain the editor shows in its Test input:
 *    analog : pin mV → moving average (filter) → calibration table
 *    digital: pin level → invert → debounce → mode
 *    → display = source × scale + offset → display filter (every disp_ms)
 *    → int subject  = round(display × 10^decimals)
 *    → text subject = display with <decimals> digits, "---" on sensor fault
 *
 *  No newlib malloc / strtof: buffers come from LVGL's own heap and numbers
 *  are parsed here, so nothing depends on newlib's re-entrancy under FreeRTOS.
 *
 *  Not handled yet: "frequency" sources (Values keep their default).
 ******************************************************************************
 */

#include "value_engine.h"
#include "board_io.h"

#include "lvgl/lvgl.h"
#include "lfs.h"
#include "lfs_port.h"            /* extern lfs */
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#define JSMN_STATIC
#include "jsmn.h"

/* =========================================================================
 * Limits
 * =========================================================================*/

#define VE_VALUES_PATH      "ui/values.json"
#define VE_MAX_VALUES       32
#define VE_MAX_CAL          32
#define VE_MAX_FILTER       64
#define VE_NAME_MAX         40
#define VE_TEXT_MAX         24
#define VE_TASK_PERIOD_MS   5U
#define VE_VDDA_PERIOD_MS   2000U

/* =========================================================================
 * Value description (from values.json) + runtime state
 * =========================================================================*/

typedef enum { SRC_NONE = 0, SRC_ANALOG, SRC_DIGITAL, SRC_FREQUENCY } ve_src_t;

typedef enum
{
    DM_STATE = 0, DM_TOGGLE, DM_LOOP, DM_UPDOWN, DM_LONGPRESS,
    DM_COUNT_RISING, DM_COUNT_FALLING, DM_COUNT_CHANGE
} ve_dmode_t;

typedef struct
{
    /* ---- configuration ---- */
    char          name[VE_NAME_MAX];
    lv_subject_t *subj;            /* int subject (always)               */
    lv_subject_t *text;            /* "<name>_s" string subject or NULL  */
    uint8_t       decimals;
    float         def;
    ve_src_t      src;
    int           pin;             /* board_io index                     */

    uint16_t      sample_ms;
    uint8_t       filter;
    uint8_t       cal_n;
    float         cal_v[VE_MAX_CAL];
    float         cal_y[VE_MAX_CAL];
    bool          fault_on;
    float         fault_lo, fault_hi;

    bool          invert;
    uint16_t      debounce_ms;
    ve_dmode_t    dmode;
    uint8_t       stages;
    uint16_t      hold_ms;

    float         scale, offset;
    uint16_t      disp_ms;
    uint16_t      disp_filter_ms;

    /* ---- runtime ---- */
    uint32_t      next_sample;
    uint32_t      next_disp;
    float         fbuf[VE_MAX_FILTER];
    uint8_t       fidx, fcount;
    float         fsum;
    volatile float value;          /* display units, full rate (written by the hardware task) */
    float         shown;           /* after the display filter           */
    volatile bool have_value;
    volatile bool faulted;
    int32_t       last_int;
    char          last_text[VE_TEXT_MAX];

    /* digital */
    bool          d_candidate, d_stable;
    uint32_t      d_changed_at, d_pressed_at, d_repeat_at;
    int32_t       d_out;
    int8_t        d_dir;
} ve_value_t;

static ve_value_t s_values[VE_MAX_VALUES];
static int        s_count;
static bool       s_need_task;

/* Debugger-visible diagnostics (Expressions window) */
volatile int      ve_load_result   = -1;  /* ve_result_t of loading values.json */
volatile int      ve_loaded        = 0;   /* values taken from values.json    */
volatile int      ve_skipped       = 0;   /* values ignored (see ve_last_skip) */
volatile char     ve_last_skip[64] = "";
volatile uint32_t ve_last_mv       = 0;   /* last analog reading, mV          */
volatile int32_t  ve_last_int      = 0;   /* last int written to a subject    */
volatile uint32_t ve_loops         = 0;   /* engine task iterations           */

/* Last step the engine reached — if the board stops, this says where:
 *   10 load started          20 values.json read      30 JSON parsed
 *   40 values set up         50 UI timer created
 *   60 hardware task started 70 ADC calibrated
 *   81 reading a pin         82 pin read
 *   83 updating the UI       84 UI updated                             */
volatile int      ve_stage         = 0;

/* Hardware task: stack and control block are STATIC (not from the FreeRTOS
 * heap, which the LVGL / GPU code shares). It only runs HAL ADC/GPIO code,
 * so a small stack is enough — all LVGL work happens in the LVGL timer. */
#define VE_STACK_BYTES   (3U * 1024U)
#define VE_UI_PERIOD_MS  10U

static osThreadId_t s_task;
static StaticTask_t s_task_cb;
static uint64_t     s_task_stack[VE_STACK_BYTES / sizeof(uint64_t)];   /* 8-byte aligned */

static const osThreadAttr_t s_task_attr =
{
    .name       = "valueEngine",
    .priority   = (osPriority_t)osPriorityBelowNormal,
    .cb_mem     = &s_task_cb,
    .cb_size    = sizeof(s_task_cb),
    .stack_mem  = s_task_stack,
    .stack_size = sizeof(s_task_stack)
};

/* FreeRTOS heap left when the engine task was created (debugger). */
volatile uint32_t ve_heap_free = 0;

/* =========================================================================
 * JSON helpers (jsmn tokens)
 * =========================================================================*/

typedef struct
{
    const char *js;
    jsmntok_t  *t;
    int         n;
} ve_json_t;

/* Index just past token i, including all its children. */
static int _skip(const ve_json_t *j, int i)
{
    int end = j->t[i].end;
    i++;
    while (i < j->n && j->t[i].start < end) i++;
    return i;
}

static bool _tok_eq(const ve_json_t *j, int i, const char *s)
{
    int len = j->t[i].end - j->t[i].start;
    return j->t[i].type == JSMN_STRING
        && (int)strlen(s) == len
        && strncmp(j->js + j->t[i].start, s, (size_t)len) == 0;
}

/* Token index of key's value inside object obj, or -1. */
static int _find(const ve_json_t *j, int obj, const char *key)
{
    if (obj < 0 || j->t[obj].type != JSMN_OBJECT) return -1;
    int i = obj + 1;
    for (int k = 0; k < j->t[obj].size && i < j->n; k++)
    {
        if (_tok_eq(j, i, key)) return i + 1;
        i = _skip(j, i + 1);     /* skip key's value */
    }
    return -1;
}

static void _str(const ve_json_t *j, int obj, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    int v = _find(j, obj, key);
    if (v < 0 || j->t[v].type != JSMN_STRING) return;
    size_t len = (size_t)(j->t[v].end - j->t[v].start);
    if (len >= cap) len = cap - 1U;
    memcpy(out, j->js + j->t[v].start, len);
    out[len] = '\0';
}

/* JSON number → float without strtof: [-]digits[.digits][e[+-]digits].
 * true → 1, false / null → 0. */
static float _tok_num(const ve_json_t *j, int v, float def)
{
    if (v < 0 || j->t[v].type != JSMN_PRIMITIVE) return def;
    const char *p   = j->js + j->t[v].start;
    const char *end = j->js + j->t[v].end;

    if (*p == 't') return 1.0f;
    if (*p == 'f' || *p == 'n') return 0.0f;

    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }

    double val = 0.0;
    bool   any = false;
    while (p < end && *p >= '0' && *p <= '9') { val = val * 10.0 + (*p - '0'); p++; any = true; }
    if (p < end && *p == '.')
    {
        p++;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') { val += (*p - '0') * scale; scale *= 0.1; p++; any = true; }
    }
    if (!any) return def;
    if (p < end && (*p == 'e' || *p == 'E'))
    {
        p++;
        bool eneg = false;
        if (p < end && (*p == '-' || *p == '+')) { eneg = (*p == '-'); p++; }
        int e = 0;
        while (p < end && *p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); p++; }
        while (e-- > 0) val = eneg ? val / 10.0 : val * 10.0;
    }
    return (float)(neg ? -val : val);
}

static float _num(const ve_json_t *j, int obj, const char *key, float def)
{
    return _tok_num(j, _find(j, obj, key), def);
}

static bool _bool(const ve_json_t *j, int obj, const char *key)
{
    return _num(j, obj, key, 0.0f) != 0.0f;
}

static int _num_array(const ve_json_t *j, int obj, const char *key, float *out, int cap)
{
    int a = _find(j, obj, key);
    if (a < 0 || j->t[a].type != JSMN_ARRAY) return 0;
    int n = 0, i = a + 1;
    for (int k = 0; k < j->t[a].size && i < j->n; k++)
    {
        if (n < cap) out[n++] = _tok_num(j, i, 0.0f);
        i = _skip(j, i);
    }
    return n;
}

/* =========================================================================
 * Loading
 * =========================================================================*/

static void _skip_value(const char *name, const char *why)
{
    ve_skipped++;
    char tmp[64];
    lv_snprintf(tmp, sizeof(tmp), "%s: %s", name, why);
    strncpy((char *)ve_last_skip, tmp, sizeof(ve_last_skip) - 1U);
}

static board_pull_t _pull(const char *s)
{
    if (strcmp(s, "up") == 0)   return BOARD_PULL_UP;
    if (strcmp(s, "down") == 0) return BOARD_PULL_DOWN;
    return BOARD_PULL_NONE;
}

static ve_dmode_t _dmode(const char *s)
{
    if (strcmp(s, "toggle") == 0)        return DM_TOGGLE;
    if (strcmp(s, "loop") == 0)          return DM_LOOP;
    if (strcmp(s, "updown") == 0)        return DM_UPDOWN;
    if (strcmp(s, "longpress") == 0)     return DM_LONGPRESS;
    if (strcmp(s, "count_rising") == 0)  return DM_COUNT_RISING;
    if (strcmp(s, "count_falling") == 0) return DM_COUNT_FALLING;
    if (strcmp(s, "count_change") == 0)  return DM_COUNT_CHANGE;
    return DM_STATE;
}

static void _load_value(const ve_json_t *j, int obj)
{
    if (s_count >= VE_MAX_VALUES)
    {
        _skip_value("?", "too many values");
        return;
    }

    ve_value_t *v = &s_values[s_count];
    memset(v, 0, sizeof(*v));

    _str(j, obj, "name", v->name, sizeof(v->name));
    if (v->name[0] == '\0') { _skip_value("?", "no name"); return; }

    v->subj = lv_xml_get_subject(NULL, v->name);
    if (v->subj == NULL) { _skip_value(v->name, "no subject in globals.xml"); return; }

    char text_name[VE_NAME_MAX + 4];
    _str(j, obj, "text", text_name, sizeof(text_name));
    v->text = text_name[0] ? lv_xml_get_subject(NULL, text_name) : NULL;

    v->decimals = (uint8_t)_num(j, obj, "decimals", 0.0f);
    if (v->decimals > 6U) v->decimals = 6U;
    v->def    = _num(j, obj, "default", 0.0f);
    v->scale  = _num(j, obj, "scale", 1.0f);
    v->offset = _num(j, obj, "offset", 0.0f);
    v->disp_ms        = (uint16_t)_num(j, obj, "disp_ms", 100.0f);
    v->disp_filter_ms = (uint16_t)_num(j, obj, "disp_filter_ms", 0.0f);
    if (v->disp_ms < 10U) v->disp_ms = 10U;

    char s[24];
    _str(j, obj, "source", s, sizeof(s));
    v->src = (strcmp(s, "analog") == 0)    ? SRC_ANALOG
           : (strcmp(s, "digital") == 0)   ? SRC_DIGITAL
           : (strcmp(s, "frequency") == 0) ? SRC_FREQUENCY
           :                                 SRC_NONE;

    char pin[12], pull[8];
    _str(j, obj, "pin", pin, sizeof(pin));
    _str(j, obj, "pull", pull, sizeof(pull));
    v->pin = board_io_find(pin);

    switch (v->src)
    {
        case SRC_ANALOG:
            if (!board_io_setup_analog(v->pin)) { _skip_value(v->name, "pin is not analog on this board"); return; }
            v->sample_ms = (uint16_t)_num(j, obj, "sample_ms", 50.0f);
            v->filter    = (uint8_t)_num(j, obj, "filter", 1.0f);
            if (v->filter < 1U) v->filter = 1U;
            if (v->filter > VE_MAX_FILTER) v->filter = VE_MAX_FILTER;
            v->cal_n = (uint8_t)_num_array(j, obj, "cal_v", v->cal_v, VE_MAX_CAL);
            if (_num_array(j, obj, "cal_y", v->cal_y, VE_MAX_CAL) != v->cal_n || v->cal_n < 2U)
            {
                _skip_value(v->name, "bad calibration table");
                return;
            }
            v->fault_on = _bool(j, obj, "fault");
            v->fault_lo = _num(j, obj, "fault_lo", 0.0f);
            v->fault_hi = _num(j, obj, "fault_hi", 1000.0f);
            s_need_task = true;
            break;

        case SRC_DIGITAL:
            if (!board_io_setup_digital(v->pin, _pull(pull))) { _skip_value(v->name, "unknown digital pin"); return; }
            v->invert      = _bool(j, obj, "invert");
            v->debounce_ms = (uint16_t)_num(j, obj, "debounce_ms", 50.0f);
            _str(j, obj, "mode", s, sizeof(s));
            v->dmode   = _dmode(s);
            v->stages  = (uint8_t)_num(j, obj, "stages", 4.0f);
            if (v->stages < 2U) v->stages = 2U;
            v->hold_ms = (uint16_t)_num(j, obj, "hold_ms", 2000.0f);
            v->sample_ms = 1U;                       /* debounce needs fast polling */
            v->d_dir = 1;
            s_need_task = true;
            break;

        case SRC_FREQUENCY:
            _skip_value(v->name, "frequency inputs not supported yet");
            /* keep it loaded as a constant so its default is still shown */
            v->src = SRC_NONE;
            break;

        default:
            break;
    }

    v->value      = v->def;
    v->shown      = v->def;
    v->last_int   = INT32_MIN;
    v->last_text[0] = '\0';
    s_count++;
    ve_loaded = s_count;
}

static void _ui_timer_cb(lv_timer_t *t);

ve_result_t value_engine_load(void)
{
    s_count = 0;
    s_need_task = false;
    ve_loaded = 0;
    ve_skipped = 0;
    ve_stage = 10;

    struct lfs_info info;
    if (lfs_stat(&lfs, VE_VALUES_PATH, &info) != LFS_ERR_OK || info.size == 0U)
    {
        ve_load_result = VE_NO_FILE;
        return VE_NO_FILE;
    }

    char *buf = lv_malloc(info.size + 1U);
    if (buf == NULL) { ve_load_result = VE_NO_MEMORY; return VE_NO_MEMORY; }

    lfs_file_t f;
    if (lfs_file_open(&lfs, &f, VE_VALUES_PATH, LFS_O_RDONLY) != LFS_ERR_OK)
    {
        lv_free(buf);
        ve_load_result = VE_READ_ERROR;
        return VE_READ_ERROR;
    }
    lfs_ssize_t got = lfs_file_read(&lfs, &f, buf, info.size);
    lfs_file_close(&lfs, &f);
    if (got != (lfs_ssize_t)info.size)
    {
        lv_free(buf);
        ve_load_result = VE_READ_ERROR;
        return VE_READ_ERROR;
    }
    buf[info.size] = '\0';
    ve_stage = 20;

    /* Pass 1: count tokens. Pass 2: parse into exactly that many. */
    jsmn_parser p;
    jsmn_init(&p);
    int ntok = jsmn_parse(&p, buf, info.size, NULL, 0);
    if (ntok <= 0) { lv_free(buf); ve_load_result = VE_PARSE_ERROR; return VE_PARSE_ERROR; }

    jsmntok_t *tok = lv_malloc(sizeof(jsmntok_t) * (size_t)ntok);
    if (tok == NULL) { lv_free(buf); ve_load_result = VE_NO_MEMORY; return VE_NO_MEMORY; }

    jsmn_init(&p);
    ntok = jsmn_parse(&p, buf, info.size, tok, (unsigned int)ntok);
    if (ntok <= 0 || tok[0].type != JSMN_OBJECT)
    {
        lv_free(tok);
        lv_free(buf);
        ve_load_result = VE_PARSE_ERROR;
        return VE_PARSE_ERROR;
    }

    ve_stage = 30;

    ve_json_t j = { buf, tok, ntok };
    int arr = _find(&j, 0, "values");
    if (arr >= 0 && tok[arr].type == JSMN_ARRAY)
    {
        int i = arr + 1;
        for (int k = 0; k < tok[arr].size && i < ntok; k++)
        {
            if (tok[i].type == JSMN_OBJECT) _load_value(&j, i);
            i = _skip(&j, i);
        }
    }

    ve_stage = 40;
    lv_free(tok);
    lv_free(buf);

    if (s_count > 0)
    {
        lv_timer_create(_ui_timer_cb, VE_UI_PERIOD_MS, NULL);
        ve_stage = 50;
    }

    ve_load_result = VE_OK;
    return VE_OK;
}

/* =========================================================================
 * Runtime
 * =========================================================================*/

static float _calibrate(const ve_value_t *v, float volts)
{
    const float *x = v->cal_v, *y = v->cal_y;
    int n = v->cal_n;
    if (volts <= x[0])     return y[0];
    if (volts >= x[n - 1]) return y[n - 1];
    for (int i = 1; i < n; i++)
    {
        if (volts <= x[i])
        {
            float span = x[i] - x[i - 1];
            if (span <= 0.0f) return y[i];
            return y[i - 1] + (volts - x[i - 1]) / span * (y[i] - y[i - 1]);
        }
    }
    return y[n - 1];
}

static void _sample_analog(ve_value_t *v)
{
    uint32_t mv;
    if (!board_io_read_mv(v->pin, &mv)) return;
    ve_last_mv = mv;

    float volts = (float)mv / 1000.0f;

    /* moving average over the last <filter> samples */
    if (v->fcount == v->filter) v->fsum -= v->fbuf[v->fidx];
    else v->fcount++;
    v->fbuf[v->fidx] = volts;
    v->fsum += volts;
    v->fidx = (uint8_t)((v->fidx + 1U) % v->filter);
    float avg = v->fsum / (float)v->fcount;

    v->faulted = v->fault_on && (avg < v->fault_lo || avg > v->fault_hi);
    if (!v->faulted)
    {
        v->value = _calibrate(v, avg) * v->scale + v->offset;
        v->have_value = true;
    }
}

/* Debounced digital input + Plex-style modes. "Holding" repeats the step
 * every 4 × debounce for toggle/loop/updown, like the SDM. */
static void _sample_digital(ve_value_t *v, uint32_t now)
{
    bool raw = board_io_read_level(v->pin) ^ v->invert;

    if (raw != v->d_candidate)
    {
        v->d_candidate  = raw;
        v->d_changed_at = now;
    }

    bool rising = false, falling = false;
    if (v->d_stable != v->d_candidate && (now - v->d_changed_at) >= v->debounce_ms)
    {
        v->d_stable = v->d_candidate;
        rising  = v->d_stable;
        falling = !v->d_stable;
        if (rising)
        {
            v->d_pressed_at = now;
            v->d_repeat_at  = now;
        }
    }

    bool step = rising;
    uint32_t repeat = 4U * (uint32_t)v->debounce_ms;
    if (!rising && v->d_stable && repeat > 0U && (now - v->d_repeat_at) >= repeat
        && (v->dmode == DM_TOGGLE || v->dmode == DM_LOOP || v->dmode == DM_UPDOWN))
    {
        step = true;
        v->d_repeat_at = now;
    }

    switch (v->dmode)
    {
        case DM_STATE:         v->d_out = v->d_stable ? 1 : 0; break;
        case DM_TOGGLE:        if (step) v->d_out = v->d_out ? 0 : 1; break;
        case DM_LOOP:          if (step) v->d_out = (v->d_out + 1) % v->stages; break;
        case DM_UPDOWN:
            if (step)
            {
                v->d_out += v->d_dir;
                if (v->d_out >= v->stages - 1) { v->d_out = v->stages - 1; v->d_dir = -1; }
                if (v->d_out <= 0)             { v->d_out = 0;              v->d_dir = 1;  }
            }
            break;
        case DM_LONGPRESS:     v->d_out = (v->d_stable && (now - v->d_pressed_at) >= v->hold_ms) ? 1 : 0; break;
        case DM_COUNT_RISING:  if (rising) v->d_out++; break;
        case DM_COUNT_FALLING: if (falling) v->d_out++; break;
        case DM_COUNT_CHANGE:  if (rising || falling) v->d_out++; break;
    }

    v->value = (float)v->d_out * v->scale + v->offset;
    v->have_value = true;
}

/* scaled integer → text with <decimals> digits, e.g. (-325, 2) → "-3.25".
 * Built by hand: no float printf (newlib-nano) and no width tricks needed. */
static void _format(char *out, size_t cap, int32_t scaled, uint8_t decimals)
{
    char tmp[16];
    int  n = 0;
    bool neg = scaled < 0;
    uint32_t a = neg ? (uint32_t)(-(int64_t)scaled) : (uint32_t)scaled;

    /* digits in reverse, inserting the point after <decimals> digits and
     * keeping at least one digit before it */
    int digits = 0;
    do
    {
        if (decimals > 0U && digits == decimals) tmp[n++] = '.';
        tmp[n++] = (char)('0' + (a % 10U));
        a /= 10U;
        digits++;
    } while ((a > 0U || digits <= decimals) && n < (int)sizeof(tmp) - 2);
    if (neg) tmp[n++] = '-';

    size_t o = 0;
    while (n > 0 && o + 1U < cap) out[o++] = tmp[--n];
    out[o] = '\0';
}

static void _publish(ve_value_t *v, uint32_t dt_ms)
{
    if (!v->have_value && !v->faulted) return;

    /* display low-pass filter: shown += (value − shown) × dt / (tau + dt) */
    if (v->disp_filter_ms > 0U && dt_ms > 0U)
        v->shown += (v->value - v->shown) * (float)dt_ms / (float)(v->disp_filter_ms + dt_ms);
    else
        v->shown = v->value;

    float  mul = 1.0f;
    for (uint8_t i = 0; i < v->decimals; i++) mul *= 10.0f;
    float  f   = v->shown * mul;
    int32_t iv = (f >= 2147483000.0f) ? INT32_MAX
               : (f <= -2147483000.0f) ? INT32_MIN + 1
               : (int32_t)(f >= 0.0f ? f + 0.5f : f - 0.5f);

    char txt[VE_TEXT_MAX];
    if (v->faulted) strcpy(txt, "---");
    else _format(txt, sizeof(txt), iv, v->decimals);

    bool int_changed  = !v->faulted && iv != v->last_int;
    bool text_changed = v->text != NULL && strcmp(txt, v->last_text) != 0;
    if (!int_changed && !text_changed) return;

    /* Runs in the LVGL task (timer) — no lock needed. */
    if (int_changed)  lv_subject_set_int(v->subj, iv);
    if (text_changed) lv_subject_copy_string(v->text, txt);

    if (int_changed) { v->last_int = iv; ve_last_int = iv; }
    if (text_changed) strcpy(v->last_text, txt);
}

/* LVGL timer: pushes the values computed by the hardware task into the
 * subjects, each at its own Display refresh rate. */
static void _ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();

    for (int i = 0; i < s_count; i++)
    {
        ve_value_t *v = &s_values[i];
        if (v->src == SRC_NONE) continue;    /* constants / set by the UI */

        if ((int32_t)(now - v->next_disp) >= 0)
        {
            ve_stage = 83;
            _publish(v, v->disp_ms);
            v->next_disp = now + v->disp_ms;
            ve_stage = 84;
        }
    }
}

/* Hardware task: ADC calibration, then sampling. Never calls LVGL. */
static void _task(void *arg)
{
    (void)arg;
    ve_stage = 60;
    //board_io_init();               /* ADC calibration + VDDA */
    ve_stage = 70;

    uint32_t next_vdda = 0U;

    for (;;)
    {
        uint32_t now = HAL_GetTick();

        if ((int32_t)(now - next_vdda) >= 0)
        {
            board_io_update_vdda();
            next_vdda = now + VE_VDDA_PERIOD_MS;
        }

        for (int i = 0; i < s_count; i++)
        {
            ve_value_t *v = &s_values[i];
            if (v->src == SRC_NONE) continue;

            if ((int32_t)(now - v->next_sample) >= 0)
            {
                ve_stage = 81;
                if (v->src == SRC_ANALOG)       _sample_analog(v);
                else if (v->src == SRC_DIGITAL) _sample_digital(v, now);
                v->next_sample = now + v->sample_ms;
                ve_stage = 82;
            }
        }

        ve_loops++;
        osDelay(VE_TASK_PERIOD_MS);
    }
}

void value_engine_start(void)
{
    if (s_task != NULL || !s_need_task) return;   /* nothing to read */
    s_task = osThreadNew(_task, NULL, &s_task_attr);
    ve_heap_free = (uint32_t)xPortGetFreeHeapSize();
}
