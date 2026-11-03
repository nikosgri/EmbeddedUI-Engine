/**
 ******************************************************************************
 * @file    value_engine.h
 * @brief   Fills the UI's Values (LVGL subjects) from the hardware.
 *
 *  The editor uploads two things:
 *    ui/globals.xml  — declares one int subject per Value (and a "<name>_s"
 *                      string subject when a label shows it)
 *    ui/values.json  — says where each Value comes from and how to compute
 *                      it (pin, filter, calibration table, scale/offset…)
 *
 *  Widgets are bound to the subjects in the XML (bind_value / bind_text /
 *  bind_state_if_*), so the engine only has to keep the subjects updated —
 *  no per-UI code, ever.
 *
 *  Usage:
 *      main.c, right after a successful ui_loader_render():
 *          value_engine_load();     reads ui/values.json, finds the subjects,
 *                                   sets the pins up, creates an LVGL timer
 *      app_freertos.c, MX_FREERTOS_Init(), after usb_at_init():
 *          value_engine_start();    starts the hardware task (ADC sampling)
 *
 *  The hardware task never calls LVGL; the LVGL timer (inside the LVGL task)
 *  is the only place subjects are written. ADC work only starts once the
 *  scheduler runs — before osKernelStart() FreeRTOS keeps interrupts masked
 *  (HAL_GetTick frozen, ADC timeouts never expire, USB can't enumerate).
 ******************************************************************************
 */
#ifndef VALUE_ENGINE_H
#define VALUE_ENGINE_H

#include <stdint.h>

typedef enum
{
    VE_OK = 0,
    VE_NO_FILE,        /* ui/values.json missing — UI without Values, not an error */
    VE_READ_ERROR,
    VE_PARSE_ERROR,
    VE_NO_MEMORY
} ve_result_t;

/** Reads ui/values.json and connects it to the subjects in globals.xml.
 *  Call in main() after ui_loader_render() succeeded, before the scheduler.
 *  No ADC access here. Result also in ve_load_result (debugger). */
ve_result_t value_engine_load(void);

/** Starts the hardware task. Call from MX_FREERTOS_Init(), after
 *  usb_at_init(). Does nothing if no Value reads the hardware. */
void value_engine_start(void);

#endif /* VALUE_ENGINE_H */
