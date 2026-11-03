/**
 ******************************************************************************
 * @file    board_io.h
 * @brief   Board-specific input layer for the Value engine.
 *
 *  The editor and values.json only know LOGICAL pin ids ("AN1", "DI3").
 *  This module is the ONLY place that knows which MCU pin / ADC channel each
 *  id is on this board. A new board = a new board_io.c with its own table;
 *  value_engine.c and the editor stay the same.
 *
 *  The ids must match the "Pins" list of this board in the editor's
 *  devices.json.
 ******************************************************************************
 */
#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    BOARD_PULL_NONE = 0,
    BOARD_PULL_UP,
    BOARD_PULL_DOWN
} board_pull_t;

/** Calibrates the ADCs. Call once, before the scheduler starts. */
void board_io_init(void);

/** Index of a logical pin id in this board's table, or -1 if unknown. */
int  board_io_find(const char *pin_id);

/** True if the pin can be read as analog / digital. */
bool board_io_is_analog(int pin);
bool board_io_is_digital(int pin);

/** Puts the pin in analog mode (call once when a Value uses it as analog). */
bool board_io_setup_analog(int pin);

/** Puts the pin in input mode with the given pull (digital use). */
bool board_io_setup_digital(int pin, board_pull_t pull);

/**
 * One analog conversion. Returns the voltage at the CONNECTOR in millivolts
 * (ADC reading × measured VDDA × the pin's input divider).
 * Blocking, a few microseconds. Call from one task only.
 */
bool board_io_read_mv(int pin, uint32_t *out_mv);

/** Raw digital level of the pin (true = high). */
bool board_io_read_level(int pin);

/** Re-measures VDDA through VREFINT (call every few seconds). */
void board_io_update_vdda(void);

/** Last measured VDDA in mV (3300 until the first measurement). */
uint32_t board_io_vdda_mv(void);

#endif /* BOARD_IO_H */
