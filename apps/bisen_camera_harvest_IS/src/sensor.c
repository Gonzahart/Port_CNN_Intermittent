//
// sensor.c - ADG732 mux + ADC scanning (compiled as C so the AmbiqSuite
// designated-initializer macros work; C++ rejects them).
//
#include "sensor.h"
#include "adc_shared.h"
#include "power.h"
#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

static const uint32_t g_SCOL[5] = { 96, 95, 98, 99, 102 };  // A0..A4 (LSB first)
static const uint32_t g_SROW[5] = {  9,  8, 10, 11,  91 };  // A0..A4 (LSB first)
static const uint32_t g_LED[3]  = { 90, 30, 97 };           // LED0,LED1,LED2 (active low)
#define PIN_CS        36
#define PIN_WR        35
#define PIN_EN        34
#define PIN_OUTCOL    100
// The ADC is no longer owned here. Apollo4 has ONE general-purpose ADC and
// the supply divider needs it too, so a single owner (adc_shared.c) holds the
// handle and hands out conversions. The photodiode is still GPIO15/SE4; that
// pad is configured in adc_shared_init() along with the divider pad.

static const uint8_t colfix[32] = {
    16,17,18,19,20,21,22,23, 24,25,26,27,28,29,30,31,
     0, 1, 2, 3, 4, 5, 6, 7,  8, 9,10,11,12,13,14,15
};
static const uint8_t rowfix[32] = {
     0, 1, 2, 3, 4, 5, 6, 7,  8, 9,10,11,12,13,14,15,
    31,30,29,28,27,26,25,24, 23,22,21,20,19,18,17,16
};

static inline void gpio_write(uint32_t pin, uint32_t val) {
    am_hal_gpio_state_write(pin,
        val ? AM_HAL_GPIO_OUTPUT_SET : AM_HAL_GPIO_OUTPUT_CLEAR);
}

static void selectPixel(uint8_t col, uint8_t row) {
    uint8_t realcol = colfix[col];
    uint8_t realrow = rowfix[row];
    gpio_write(PIN_WR, 0);
    for (int b = 0; b < 5; b++) {
        gpio_write(g_SCOL[b], (realcol >> b) & 1);
        gpio_write(g_SROW[b], (realrow >> b) & 1);
    }
    // The dominant cost of a frame: 3.5 ms per pixel, and today it is spent
    // spinning at the core clock. Attributed so the energy report shows it.
    pwr_phase_t prev = pwr_begin(PWR_SETTLE);
    pwr_wait_us(2500);
    gpio_write(PIN_WR, 1);
    pwr_wait_us(1000);
    pwr_end(prev);
}

static void gpio_init(void) {
    am_hal_gpio_pincfg_t sOut = AM_HAL_GPIO_PINCFG_OUTPUT;
    for (int i = 0; i < 5; i++) { am_hal_gpio_pinconfig(g_SCOL[i], sOut); gpio_write(g_SCOL[i], 0); }
    for (int i = 0; i < 5; i++) { am_hal_gpio_pinconfig(g_SROW[i], sOut); gpio_write(g_SROW[i], 0); }
    am_hal_gpio_pinconfig(PIN_CS,     sOut); gpio_write(PIN_CS,     0);
    am_hal_gpio_pinconfig(PIN_EN,     sOut); gpio_write(PIN_EN,     0);
    am_hal_gpio_pinconfig(PIN_WR,     sOut); gpio_write(PIN_WR,     1);
    am_hal_gpio_pinconfig(PIN_OUTCOL, sOut); gpio_write(PIN_OUTCOL, 1);
}


// The polling-vs-sleep analysis that used to live here moved with the
// conversion loop into adc_shared.c. Short version: with AVG_16 each
// trigger is one scan of ~7 us, far too short to sleep through, so polling
// is correct. Sleeping made a frame 4.26 s -> 11.17 s when it was tried.

void sensor_init(void) {
    gpio_init();
    adc_shared_init();   // single owner of ADC0: pixel slot + supply slot
    pwr_wait_init();     // wake source for the settling wait
}

void led_init(void) {
    am_hal_gpio_pincfg_t sOut = AM_HAL_GPIO_PINCFG_OUTPUT;
    for (int i = 0; i < 3; i++) {
        am_hal_gpio_pinconfig(g_LED[i], sOut);
        gpio_write(g_LED[i], 1);   // active-low: drive HIGH = off
    }
}

void led_select(int index) {
    for (int i = 0; i < 3; i++)
        gpio_write(g_LED[i], (i == index) ? 0 : 1);   // 0 = on (active low)
}



#define CROP_W   23                         // centered crop width
#define CROP_H   24                         // centered crop height
#define COL_MIN  ((32 - CROP_W) / 2)        
#define COL_MAX  (COL_MIN + CROP_W - 1)     
#define ROW_MIN  ((32 - CROP_H) / 2)        
#define ROW_MAX  (ROW_MIN + CROP_H - 1)     

// Crop ("heatmap") toggle. false = full 32x32 normal capture (default);
// true = mask to the centered crop box
// (CROP_W x CROP_H below, currently 23x24). Lives here so it persists across
// the scan-mode changes driven from cam_lenet.cc.
static bool g_crop = false;

void sensor_set_crop(bool on) { g_crop = on; }
bool sensor_get_crop(void)    { return g_crop; }

static int in_mask(int col, int row) {
    return (col >= COL_MIN && col <= COL_MAX &&
            row >= ROW_MIN && row <= ROW_MAX);
}

static uint32_t adc_read_once_timed(void) {
    pwr_phase_t prev = pwr_begin(PWR_ADC);
    uint32_t v = adc_shared_read_pixel();
    pwr_end(prev);
    return v;
}

uint16_t read_pixel_raw(uint8_t col, uint8_t row) {
    selectPixel(col, row);
    return (uint16_t)adc_read_once_timed();
}

uint16_t read_pixel(uint8_t col, uint8_t row) {
    if (g_crop && !in_mask(col, row)) {
        return PIXEL_SKIPPED;         // heatmap on: skip pixels outside the NxN box
    }
    return read_pixel_raw(col, row);
}
