#ifndef BISEN_HARVEST_INPUT_H
#define BISEN_HARVEST_INPUT_H

#include <stdint.h>

// Physical VCAP conversion and virtual policy mapping are intentionally
// separate. These 1.90/2.00/2.10/2.20 V values are POLICY coordinates, not
// voltages to look for at the 5-8 V reservoir.
#ifndef BISEN_TRACE_CALIBRATION_MODE
#define BISEN_TRACE_CALIBRATION_MODE 1
#endif
#ifndef BISEN_HARVEST_DIVIDER_TOP_OHM
#define BISEN_HARVEST_DIVIDER_TOP_OHM 390000
#endif
#ifndef BISEN_HARVEST_DIVIDER_BOTTOM_OHM
#define BISEN_HARVEST_DIVIDER_BOTTOM_OHM 10000
#endif
#ifndef BISEN_HARVEST_MAX_VCAP_UV
#define BISEN_HARVEST_MAX_VCAP_UV 8000000
#endif
#ifndef BISEN_HARVEST_CAL_LOW_CODE
#define BISEN_HARVEST_CAL_LOW_CODE 0
#endif
#ifndef BISEN_HARVEST_CAL_LOW_UV
#define BISEN_HARVEST_CAL_LOW_UV 0
#endif
#ifndef BISEN_HARVEST_CAL_HIGH_CODE
#define BISEN_HARVEST_CAL_HIGH_CODE 4095
#endif
#ifndef BISEN_HARVEST_CAL_HIGH_UV
#define BISEN_HARVEST_CAL_HIGH_UV 0
#endif
#ifndef BISEN_HARVEST_CRITICAL_UV
#define BISEN_HARVEST_CRITICAL_UV 0
#endif
#ifndef BISEN_HARVEST_WORK100_UV
#define BISEN_HARVEST_WORK100_UV 0
#endif
#ifndef BISEN_HARVEST_WORK500_UV
#define BISEN_HARVEST_WORK500_UV 0
#endif
#ifndef BISEN_HARVEST_WORK1000_UV
#define BISEN_HARVEST_WORK1000_UV 0
#endif
#ifndef BISEN_HARVEST_SWITCHED_DIVIDER
#define BISEN_HARVEST_SWITCHED_DIVIDER 0
#endif

#if BISEN_HARVEST_DIVIDER_TOP_OHM <= 0 || BISEN_HARVEST_DIVIDER_BOTTOM_OHM <= 0
#error "VCAP divider resistors must be positive"
#endif
// Use a 1.10 V design limit below the Apollo4 ADC's 1.19 V conversion
// reference. Firmware cannot electrically protect an overdriven input.
#if ((BISEN_HARVEST_MAX_VCAP_UV / 1000) * BISEN_HARVEST_DIVIDER_BOTTOM_OHM) / \
    (BISEN_HARVEST_DIVIDER_TOP_OHM + BISEN_HARVEST_DIVIDER_BOTTOM_OHM) > 1100
#error "Configured VCAP maximum drives ADCSE3 above the 1.10 V design limit"
#endif
// A fixed divider cannot meet the ADC's 0..VDDH safe-input specification
// while VDDH is zero. Limit its unpowered voltage to 250 mV with a 5% resistor
// tolerance calculation, below the GPIO absolute maximum VDDH+300 mV.
// This is a conservative bench limit, not a claim of powered-off ADC operation.
#define BISEN_HARVEST_FIXED_WORST_MV \
    ((BISEN_HARVEST_MAX_VCAP_UV / 1000LL) * 105LL * \
     BISEN_HARVEST_DIVIDER_BOTTOM_OHM / \
     (95LL * BISEN_HARVEST_DIVIDER_TOP_OHM + \
      105LL * BISEN_HARVEST_DIVIDER_BOTTOM_OHM))
#if !BISEN_HARVEST_SWITCHED_DIVIDER && \
    BISEN_HARVEST_FIXED_WORST_MV > 250
#error "Fixed divider exceeds the 250 mV unpowered-pin bench limit"
#endif
#if BISEN_HARVEST_CAL_LOW_CODE < 0 || BISEN_HARVEST_CAL_HIGH_CODE > 4095 || \
    BISEN_HARVEST_CAL_HIGH_CODE <= BISEN_HARVEST_CAL_LOW_CODE
#error "Invalid VCAP ADC calibration codes"
#endif
#if BISEN_HARVEST_CAL_HIGH_UV != 0 && \
    BISEN_HARVEST_CAL_HIGH_UV <= BISEN_HARVEST_CAL_LOW_UV
#error "Calibrated VCAP voltage pairs must be ordered"
#endif

#if !BISEN_TRACE_CALIBRATION_MODE
#if BISEN_HARVEST_CRITICAL_UV <= 0 || \
    BISEN_HARVEST_WORK100_UV <= BISEN_HARVEST_CRITICAL_UV || \
    BISEN_HARVEST_WORK500_UV <= BISEN_HARVEST_WORK100_UV || \
    BISEN_HARVEST_WORK1000_UV <= BISEN_HARVEST_WORK500_UV || \
    BISEN_HARVEST_WORK1000_UV >= BISEN_HARVEST_MAX_VCAP_UV
#error "Full harvest build requires measured, ordered physical VCAP thresholds"
#endif
#if BISEN_HARVEST_CAL_LOW_CODE <= 0 || BISEN_HARVEST_CAL_LOW_UV <= 0 || \
    BISEN_HARVEST_CAL_HIGH_CODE >= 4095 || BISEN_HARVEST_CAL_HIGH_UV <= 0
#error "Full harvest build requires measured VCAP/ADC calibration pairs"
#endif
#endif

// Calibration mode can display a nominal estimate before measured anchors
// exist. Full mode uses the DMM/ADC two-point fit from this exact divider.
static inline uint32_t trace_input_microvolts(uint32_t adc_code) {
#if BISEN_HARVEST_CAL_HIGH_UV == 0
    return (uint32_t)((uint64_t)adc_code * 1190000u *
        (BISEN_HARVEST_DIVIDER_TOP_OHM + BISEN_HARVEST_DIVIDER_BOTTOM_OHM) /
        (4096u * BISEN_HARVEST_DIVIDER_BOTTOM_OHM));
#else
    const int64_t delta = (int64_t)adc_code - BISEN_HARVEST_CAL_LOW_CODE;
    const int64_t uv = BISEN_HARVEST_CAL_LOW_UV + delta *
        (BISEN_HARVEST_CAL_HIGH_UV - BISEN_HARVEST_CAL_LOW_UV) /
        (BISEN_HARVEST_CAL_HIGH_CODE - BISEN_HARVEST_CAL_LOW_CODE);
    return uv < 0 ? 0u : (uint32_t)uv;
#endif
}

static inline uint32_t harvest_segment_code(uint32_t uv,
    uint32_t lo_uv, uint32_t hi_uv, uint32_t lo_code, uint32_t hi_code) {
    return lo_code + (uint32_t)((uint64_t)(uv - lo_uv) *
        (hi_code - lo_code) / (hi_uv - lo_uv));
}

static inline uint32_t trace_policy_code(uint32_t uv) {
#if BISEN_TRACE_CALIBRATION_MODE
    (void)uv;
    return 0u; // calibration build never runs a workload
#else
    if (uv < BISEN_HARVEST_CRITICAL_UV)
        return harvest_segment_code(uv, 0u, BISEN_HARVEST_CRITICAL_UV,
                                    0u, 2185u);
    if (uv < BISEN_HARVEST_WORK100_UV)
        return harvest_segment_code(uv, BISEN_HARVEST_CRITICAL_UV,
                                    BISEN_HARVEST_WORK100_UV, 2185u, 2333u);
    if (uv < BISEN_HARVEST_WORK500_UV)
        return harvest_segment_code(uv, BISEN_HARVEST_WORK100_UV,
                                    BISEN_HARVEST_WORK500_UV, 2333u, 2441u);
    if (uv < BISEN_HARVEST_WORK1000_UV)
        return harvest_segment_code(uv, BISEN_HARVEST_WORK500_UV,
                                    BISEN_HARVEST_WORK1000_UV, 2441u, 2553u);
    return 2553u;
#endif
}

#endif
