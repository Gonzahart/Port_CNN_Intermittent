#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>
#include <stdbool.h>

// Value returned for masked-out (skipped) pixels. 0xFFFF can't occur on the
// 12-bit ADC (0..4095), so preprocessing can detect & exclude these pixels.
#define PIXEL_SKIPPED  0xFFFFu

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the mux GPIOs and the ADC.
void sensor_init(void);

// Select pixel (col,row) and return its 12-bit ADC value (0..4095).
// When the crop ("heatmap") is enabled, pixels outside the centered crop
// box (CROP_W x CROP_H in sensor.c) return PIXEL_SKIPPED; when disabled, the
// full 32x32 array is scanned.
uint16_t read_pixel(uint8_t col, uint8_t row);

// Same as read_pixel but always scans, ignoring the crop toggle. Internal
// primitive that read_pixel calls; scan callers should use read_pixel.
uint16_t read_pixel_raw(uint8_t col, uint8_t row);

// Crop ("heatmap") toggle: OFF = full 32x32 normal capture (default),
// ON = mask to the centered CROP_W x CROP_H box. Persists across mode changes.
void sensor_set_crop(bool on);
bool sensor_get_crop(void);

// EVB status LEDs (active-low, GPIO 90/30/97). led_init() configures them (all
// off). led_select(i) lights only LED i (0..2); pass i<0 to turn them all off.
void led_init(void);
void led_select(int index);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_H
