#ifndef PREPROCESS_H
#define PREPROCESS_H

#include <stdint.h>

// Turn a raw 32x32 frame into the model's int8 input.
//
// Blur, then either Otsu-binarize or min-max normalize, then vertical flip and
// quantize with the model's own input scale and zero point. Pixels the sensor
// skipped (PIXEL_SKIPPED) are excluded from the statistics and land as black.
//
// Whatever happens here has to match what the model was trained on. Getting it
// wrong does not fail loudly -- accuracy just comes out mediocre.

#ifdef __cplusplus
extern "C" {
#endif

// raw_in: 1024 values, 0..4095 or PIXEL_SKIPPED, row-major 32x32.
// in_data: 1024 int8 values, ready for nn_begin().
void preprocess(const uint16_t raw_in[1024], int8_t in_data[1024]);

#ifdef __cplusplus
}
#endif

#endif  // PREPROCESS_H
