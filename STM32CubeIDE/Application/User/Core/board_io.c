/**
 ******************************************************************************
 * @file    board_io.c
 * @brief   Riverdi RVT50HQSNWA01 (STM32U5G9) input table for the Value engine.
 *
 *  Logical ids match devices.json → "RVT50HQSNWA01" → "Pins".
 *  All analog pins are on ADC1 (14-bit), read single-ended one at a time
 *  with a long sampling time (potentiometers and sensors are high-impedance
 *  sources). VDDA is measured through VREFINT on ADC2, so millivolts stay
 *  correct even if the 3.3 V rail is a little off.
 *
 *  NOTE: CubeMX configures ADC1 channel 11 as DIFFERENTIAL (PA6+, PA7−).
 *  Every read here reconfigures the channel as single-ended, so PA6 (AN1)
 *  and PA7 (AN2) work as two independent inputs without touching the .ioc.
 *
 *  Pins already used by other peripherals in this firmware (taking one of
 *  them for a Value overrides that peripheral's pin setup):
 *    PA3  USART2_RX     PA4/PA5  DAC1      PB6  I2C4_SCL   PB7  USART1_RX
 *    PB10 USART3_TX     PG2/3/4  SPI1      PG9  USART1_TX (printf!)
 *    PH4/PH5 I2C2       PB0 TIM1_CH2N      PB1/PB15 LPTIM2
 ******************************************************************************
 */

#include "board_io.h"
#include "main.h"
#include "adc.h"

#include <string.h>

/* Long sampling time for high-impedance sources (newer HAL names first). */
#if defined(ADC_SAMPLETIME_391CYCLES)
  #define BIO_SAMPLETIME   ADC_SAMPLETIME_391CYCLES
#else
  #define BIO_SAMPLETIME   ADC_SAMPLETIME_68CYCLES
#endif

#define BIO_ADC_FULL_SCALE   16383U      /* 14-bit */
#define BIO_POLL_TIMEOUT_MS  5U

typedef struct
{
    const char   *id;          /* logical id, as in devices.json            */
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint32_t      adc_channel; /* ADC1 channel, 0 = not analog-capable      */
    uint16_t      divider_x1000; /* input divider × 1000 (1000 = direct)    */
} bio_pin_t;

/* ---- Riverdi RVT50 pin table (P4 expansion connector + user button) ---- */
static const bio_pin_t s_pins[] =
{
    /* id      port   pin           ADC1 channel     divider */
    { "AN1",  GPIOA, GPIO_PIN_6,  ADC_CHANNEL_11,  1000 },   /* P4-22 */
    { "AN2",  GPIOA, GPIO_PIN_7,  ADC_CHANNEL_12,  1000 },   /* P4-20 */
    { "AN3",  GPIOA, GPIO_PIN_3,  ADC_CHANNEL_8,   1000 },   /* P4-26 */
    { "AN4",  GPIOA, GPIO_PIN_4,  ADC_CHANNEL_9,   1000 },   /* P4-8  */
    { "AN5",  GPIOA, GPIO_PIN_5,  ADC_CHANNEL_10,  1000 },   /* P4-5  */
    { "AN6",  GPIOB, GPIO_PIN_0,  ADC_CHANNEL_15,  1000 },   /* P4-33 */
    { "AN7",  GPIOB, GPIO_PIN_1,  ADC_CHANNEL_16,  1000 },   /* P4-12 */
    { "AN8",  GPIOB, GPIO_PIN_2,  ADC_CHANNEL_17,  1000 },   /* P4-30 */

    { "DI1",  GPIOH, GPIO_PIN_3,  0,               1000 },   /* user button S1 */
    { "DI2",  GPIOC, GPIO_PIN_7,  0,               1000 },   /* P4-14 */
    { "DI3",  GPIOB, GPIO_PIN_6,  0,               1000 },   /* P4-13 */
    { "DI4",  GPIOB, GPIO_PIN_7,  0,               1000 },   /* P4-15 */
    { "DI5",  GPIOB, GPIO_PIN_10, 0,               1000 },   /* P4-29 */
    { "DI6",  GPIOB, GPIO_PIN_15, 0,               1000 },   /* P4-10 */
    { "DI7",  GPIOA, GPIO_PIN_15, 0,               1000 },   /* P4-37 */
    { "DI8",  GPIOG, GPIO_PIN_2,  0,               1000 },   /* P4-23 */
    { "DI9",  GPIOG, GPIO_PIN_3,  0,               1000 },   /* P4-19 */
    { "DI10", GPIOG, GPIO_PIN_4,  0,               1000 },   /* P4-21 */
    { "DI11", GPIOG, GPIO_PIN_9,  0,               1000 },   /* P4-25 */
    { "DI12", GPIOG, GPIO_PIN_10, 0,               1000 },   /* P4-27 */
    { "DI13", GPIOH, GPIO_PIN_4,  0,               1000 },   /* P4-36 */
    { "DI14", GPIOH, GPIO_PIN_5,  0,               1000 },   /* P4-34 */
    { "DI15", GPIOH, GPIO_PIN_11, 0,               1000 },   /* P4-24 */
};

#define BIO_PIN_COUNT  ((int)(sizeof(s_pins) / sizeof(s_pins[0])))

/* Debugger-visible state (Expressions window) */
volatile uint32_t board_io_vdda      = 3300U;   /* measured VDDA, mV          */
volatile int      board_io_cal_err   = -1;      /* 0 = ADC calibration OK     */
volatile uint32_t board_io_last_raw  = 0U;      /* last raw ADC1 reading      */

/* =========================================================================
 * Public API
 * =========================================================================*/

void board_io_init(void)
{
    /* Calibrate both ADCs once, while they are still disabled. */
    int err = 0;
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) err |= 1;
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) err |= 2;
    board_io_cal_err = err;

    board_io_update_vdda();
}

int board_io_find(const char *pin_id)
{
    if (pin_id == NULL || pin_id[0] == '\0') return -1;
    for (int i = 0; i < BIO_PIN_COUNT; i++)
    {
        if (strcmp(s_pins[i].id, pin_id) == 0) return i;
    }
    return -1;
}

bool board_io_is_analog(int pin)
{
    return pin >= 0 && pin < BIO_PIN_COUNT && s_pins[pin].adc_channel != 0U;
}

bool board_io_is_digital(int pin)
{
    return pin >= 0 && pin < BIO_PIN_COUNT;
}

bool board_io_setup_analog(int pin)
{
    if (!board_io_is_analog(pin)) return false;

    GPIO_InitTypeDef g = {0};
    g.Pin  = s_pins[pin].pin;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(s_pins[pin].port, &g);
    return true;
}

bool board_io_setup_digital(int pin, board_pull_t pull)
{
    if (!board_io_is_digital(pin)) return false;

    GPIO_InitTypeDef g = {0};
    g.Pin   = s_pins[pin].pin;
    g.Mode  = GPIO_MODE_INPUT;
    g.Pull  = (pull == BOARD_PULL_UP)   ? GPIO_PULLUP
            : (pull == BOARD_PULL_DOWN) ? GPIO_PULLDOWN
            :                             GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(s_pins[pin].port, &g);
    return true;
}

/* One single-ended conversion on an ADC (polling). */
static bool _convert(ADC_HandleTypeDef *hadc, uint32_t channel, uint32_t *out_raw)
{
    ADC_ChannelConfTypeDef c = {0};
    c.Channel      = channel;
    c.Rank         = ADC_REGULAR_RANK_1;
    c.SamplingTime = BIO_SAMPLETIME;
    c.SingleDiff   = ADC_SINGLE_ENDED;
    c.OffsetNumber = ADC_OFFSET_NONE;
    c.Offset       = 0;

    if (HAL_ADC_ConfigChannel(hadc, &c) != HAL_OK) return false;
    if (HAL_ADC_Start(hadc) != HAL_OK) return false;

    bool ok = (HAL_ADC_PollForConversion(hadc, BIO_POLL_TIMEOUT_MS) == HAL_OK);
    if (ok) *out_raw = HAL_ADC_GetValue(hadc);

    (void)HAL_ADC_Stop(hadc);
    return ok;
}

bool board_io_read_mv(int pin, uint32_t *out_mv)
{
    if (!board_io_is_analog(pin) || out_mv == NULL) return false;

    uint32_t raw;
    if (!_convert(&hadc1, s_pins[pin].adc_channel, &raw)) return false;
    board_io_last_raw = raw;

    uint64_t mv = (uint64_t)raw * board_io_vdda / BIO_ADC_FULL_SCALE;
    mv = mv * s_pins[pin].divider_x1000 / 1000U;
    *out_mv = (uint32_t)mv;
    return true;
}

bool board_io_read_level(int pin)
{
    if (!board_io_is_digital(pin)) return false;
    return HAL_GPIO_ReadPin(s_pins[pin].port, s_pins[pin].pin) == GPIO_PIN_SET;
}

void board_io_update_vdda(void)
{
    /* VDDA = VREFINT_CAL_VREF × VREFINT_CAL / VREFINT reading (both 14-bit). */
    uint32_t raw;
    if (!_convert(&hadc2, ADC_CHANNEL_VREFINT, &raw) || raw == 0U) return;

    uint32_t cal  = (uint32_t)(*VREFINT_CAL_ADDR);
    uint32_t vdda = (uint32_t)((uint64_t)VREFINT_CAL_VREF * cal / raw);

    /* Ignore readings that can't be a real supply (bad channel, no cal). */
    if (vdda >= 2400U && vdda <= 3600U)
    {
        board_io_vdda = vdda;
    }
}

uint32_t board_io_vdda_mv(void)
{
    return board_io_vdda;
}
