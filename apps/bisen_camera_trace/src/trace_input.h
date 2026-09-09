#ifndef BISEN_TRACE_INPUT_H
#define BISEN_TRACE_INPUT_H

#include <stdint.h>

#if defined(BISEN_CAMERA_VDD_CHUNK1000_CODE) && \
    (BISEN_CAMERA_VDD_CHUNK1000_CODE != 2553 || \
     BISEN_CAMERA_VDD_CHUNK500_CODE != 2441 || \
     BISEN_CAMERA_VDD_CHUNK100_CODE != 2333 || \
     BISEN_CAMERA_VDD_SLEEP_CODE != 2185 || BISEN_CAMERA_VDD_RESUME_CODE != 2441)
#error "Trace adapter preserves the original policy codes; calibrate the input instead"
#endif

// ADCSE3 measures a 20 kohm / 10 kohm divider. Defaults are NOMINAL,
// not a transfer of the internal-BATT calibration. Calibrate against the
// actual voltage at the divider input using two measured code/voltage pairs.
#ifndef BISEN_TRACE_CAL_LOW_CODE
#define BISEN_TRACE_CAL_LOW_CODE 0
#endif
#ifndef BISEN_TRACE_CAL_HIGH_CODE
#define BISEN_TRACE_CAL_HIGH_CODE 4096
#endif
#ifndef BISEN_TRACE_CAL_LOW_UV
#define BISEN_TRACE_CAL_LOW_UV 0
#endif
#ifndef BISEN_TRACE_CAL_HIGH_UV
#define BISEN_TRACE_CAL_HIGH_UV 3570000
#endif
#if BISEN_TRACE_CAL_LOW_CODE < 0 || BISEN_TRACE_CAL_HIGH_CODE > 4096 || \
    BISEN_TRACE_CAL_HIGH_CODE <= BISEN_TRACE_CAL_LOW_CODE || \
    BISEN_TRACE_CAL_LOW_UV < 0 || BISEN_TRACE_CAL_HIGH_UV > 5000000 || \
    BISEN_TRACE_CAL_HIGH_UV <= BISEN_TRACE_CAL_LOW_UV
#error "Trace calibration must contain two ordered ADC-code/voltage pairs"
#endif

static inline uint32_t trace_input_microvolts(uint32_t adc_code) {
    const int64_t delta = (int64_t)adc_code - BISEN_TRACE_CAL_LOW_CODE;
    const int64_t uv = BISEN_TRACE_CAL_LOW_UV +
        delta * (BISEN_TRACE_CAL_HIGH_UV - BISEN_TRACE_CAL_LOW_UV) /
        (BISEN_TRACE_CAL_HIGH_CODE - BISEN_TRACE_CAL_LOW_CODE);
    return uv < 0 ? 0u : (uint32_t)uv;
}

// Translate the imposed voltage into the ORIGINAL direct-VDD policy's code
// space. These are virtual codes, never claimed to be ADCSE3 or BATT samples.
// Piecewise interpolation hits every original DMM anchor exactly. Truncation
// prevents promoting a sample immediately below a threshold into that band.
// The policy, camera/CNN engine and checkpoint decisions remain unchanged.
static inline uint32_t trace_policy_code(uint32_t uv) {
    static const uint32_t volts[] = {0, 1900000, 2000000, 2100000, 2200000};
    static const uint32_t codes[] = {0, 2185, 2333, 2441, 2553};
    for (uint32_t i = 1; i < 5; ++i) {
        if (uv < volts[i]) {
            return codes[i - 1] + (uint32_t)(
                (uint64_t)(uv - volts[i - 1]) * (codes[i] - codes[i - 1]) /
                (volts[i] - volts[i - 1]));
        }
    }
    // All higher trace voltages remain in the highest policy band. This is
    // numerical saturation, NOT electrical protection of the ADC pin.
    return codes[4];
}

#endif
