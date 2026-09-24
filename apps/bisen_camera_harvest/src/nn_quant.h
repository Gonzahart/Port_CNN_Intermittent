// nn_quant.h -- fixed-point requantize, used by the kernels.
//
// A header, not a .c, on purpose: these run once per output element, ~400k
// calls an inference. In their own translation unit they stop being inlined
// unless the whole build turns on LTO.
//
// A layer accumulates in int32, then scales back to int8 with a per-channel
// (multiplier, shift) pair the host computed.
//
// Two rounding conventions exist and they differ by one LSB on exact halves.
// Which op uses which was measured, not assumed: against the TFLite reference
// kernels conv matches double rounding, per-channel dense matches single.
// Mode 2 gets all 100 vectors; modes 0 and 1 get 80 and 94. The label was the
// same under all three, so this only ever moves a logit by one -- still worth
// picking on purpose.
//
//   0 = single everywhere
//   1 = double everywhere (old gemmlowp)
//   2 = double for conv, single for dense (matches TFLite reference)
//
#ifndef NN_QUANT_H
#define NN_QUANT_H

#include <stdint.h>

#ifndef NN_ROUNDING
#define NN_ROUNDING 2
#endif

static inline int32_t requantize_single(int32_t x, int32_t mult, int32_t shift) {
    const int total_shift = 31 - shift;
    const int64_t round = (int64_t)1 << (total_shift - 1);
    int64_t result = (int64_t)x * (int64_t)mult + round;
    result >>= total_shift;
    if (result < INT32_MIN) result = INT32_MIN;
    if (result > INT32_MAX) result = INT32_MAX;
    return (int32_t)result;
}

static inline int32_t requantize_double(int32_t x, int32_t mult, int32_t shift) {
    const int left = (shift > 0) ? shift : 0;
    const int right = (shift > 0) ? 0 : -shift;
    int64_t ab = (int64_t)(x * ((int32_t)1 << left)) * (int64_t)mult;
    int64_t nudge = (ab >= 0) ? (1 << 30) : (1 - (1 << 30));
    int32_t hi = (int32_t)((ab + nudge) / ((int64_t)1 << 31));
    if (right <= 0) return hi;
    const int32_t mask = ((int32_t)1 << right) - 1;
    const int32_t remainder = hi & mask;
    const int32_t threshold = (mask >> 1) + (hi < 0 ? 1 : 0);
    return (hi >> right) + (remainder > threshold ? 1 : 0);
}

#if NN_ROUNDING == 1
#define requantize_conv requantize_double
#define requantize_fc   requantize_double
#elif NN_ROUNDING == 2
#define requantize_conv requantize_double
#define requantize_fc   requantize_single
#else
#define requantize_conv requantize_single
#define requantize_fc   requantize_single
#endif

static inline int8_t clamp_i8(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int8_t)v;
}

#endif  // NN_QUANT_H
