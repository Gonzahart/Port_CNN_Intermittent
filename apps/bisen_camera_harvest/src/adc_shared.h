#ifndef ADC_SHARED_H
#define ADC_SHARED_H

#include <stdint.h>

#ifndef BISEN_HARVEST_ADC_DIAG_COMPARE
#define BISEN_HARVEST_ADC_DIAG_COMPARE 0
#endif

// Which pad feeds the supply (VCAP divider) ADC slot.
// 17 = GPIO17/ADCSE2 (production default since 2026-10-05; scope-off tests
//      read within about 0.4% of the DMM once the measured divider ratio is used).
// 16 = J9.8/GPIO16/ADCSE3 (legacy input; not re-validated with the scope off).
// BISEN_HARVEST_DIAG_SUPPLY_PIN is the old calibration-only name, still accepted.
#ifndef BISEN_HARVEST_SUPPLY_PIN
#ifdef BISEN_HARVEST_DIAG_SUPPLY_PIN
#define BISEN_HARVEST_SUPPLY_PIN BISEN_HARVEST_DIAG_SUPPLY_PIN
#else
#define BISEN_HARVEST_SUPPLY_PIN 17
#endif
#endif

#if BISEN_HARVEST_SUPPLY_PIN == 17
#define BISEN_HARVEST_SUPPLY_PIN_NAME "GPIO17/ADCSE2"
#else
#define BISEN_HARVEST_SUPPLY_PIN_NAME "J9.8/GPIO16/ADCSE3"
#endif

// Pad the CAL_* anchors were fitted on (fit_vcap_calibration.py prints it).
// Full builds refuse anchors from a different pad, because anchors belong to
// one pad and one wiring setup (the old 461/634 GPIO16 anchors are invalid).
// 0 = not stated (only allowed in calibration builds).
#ifndef BISEN_HARVEST_CAL_PIN
#define BISEN_HARVEST_CAL_PIN 0
#endif

// Calibration-only deep ADC diagnostic (2026-10-01). After each BTN0 capture
// it records the HAL correction trims, ADC CFG/SL1CFG, and three extra passes
// on the supply input: production config with full 12.6 precision, LPMODE0
// (ADC kept powered between scans), and AVG_1 single conversions.
#ifndef BISEN_HARVEST_ADC_DIAG_DEEP
#define BISEN_HARVEST_ADC_DIAG_DEEP 0
#endif

// adc_shared.h -- the single owner of Apollo4 ADC instance 0.
//
// WHY THIS FILE EXISTS
// Apollo4 has ONE general-purpose ADC. Two pieces of code wanted to own it:
// sensor.c (photodiode readout on SE4/pin 15) and the BISen energy path (supply
// external VCAP input (GPIO17/ADCSE2 by default; see BISEN_HARVEST_SUPPLY_PIN). Both must share ADC0; a
// second am_hal_adc_initialize() owner would conflict with camera acquisition.
//
// The fix is the pattern from a working multi-sensor design: ONE owner, no
// teardown, and a private slot per consumer. Everything that touches the ADC
// goes through this file.
//
// WHY MODES RATHER THAN TWO SLOTS CONVERTING TOGETHER
// The Apollo4 ADC has one trigger for the whole scan: enabling both slots means
// every pixel conversion also converts the supply. That sounds free, and during
// the scan it nearly is -- but it makes the supply reading a BYPRODUCT OF
// COMPUTING. In the wait state, where the scheduler has stopped work and is
// waiting for the rail to recover, there are no pixel triggers, so exactly when
// the reading matters most it would stop arriving. Instead each mode enables
// exactly one slot, so there is never a FIFO to demux and the supply can be
// watched with the camera completely idle.
//
// ⚠ ERRATA ERR091 / ERR113 (datasheet p.112): only the 24 MHz HFRC clock is
// usable, and at least 37 tracking cycles are required. Both are honoured
// below. Do not "optimise" the clock selection.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADC_MODE_PIXEL = 0,   // slot 0 = SE4 photodiode, LPMODE0 (0 us scan start)
    ADC_MODE_SUPPLY,      // slot 1 = external VCAP input (SE2/SE3), LPMODE1 (53.7 us, lower power)
    ADC_MODE_WATCH,       // slot 1 + window comparator, repeating scan
} adc_mode_t;

// Initialise the peripheral once. Configures the photodiode pad and both slot
// templates, then enters ADC_MODE_PIXEL. Configures the supply pad (GPIO17/SE2 or GPIO16/SE3) once and
// allows the divider filter to settle. Call once, from sensor_init().
void adc_shared_init(void);
// Disable/power down ADC0 during long energy waits; the next read restores it.
void adc_shared_park(void);

// Which mode is currently programmed.
adc_mode_t adc_shared_mode(void);

// Switch modes. Cheap (disable / configure / enable, a few microseconds) and
// idempotent -- asking for the mode already programmed does nothing.
void adc_shared_set_mode(adc_mode_t mode);

// One photodiode conversion. Requires ADC_MODE_PIXEL; the caller is sensor.c.
uint32_t adc_shared_read_pixel(void);

// One robust external-trace decision sample. Switches to ADC_MODE_SUPPLY,
// discards the first averaged result after the mode switch, returns the median
// of the next three hardware-AVG16 results, and restores the previous mode.
// This rejects one isolated post-switch outlier without policy hysteresis.
// Returns 0 on success and writes the raw median code to *out_code; returns -1
// and leaves *out_code alone on failure.
//
// The CALLER converts the code to millivolts. This file deliberately does not,
// so the bench calibration stays in bisen_config.h where its author put it.
int adc_shared_read_supply(uint32_t *out_code);

#if BISEN_HARVEST_ADC_DIAG_COMPARE
typedef struct {
    uint32_t read_failures;
    uint32_t empty_reads;
    uint32_t wrong_slots;
    uint32_t drain_failures;
} adc_shared_diag_stats_t;

void adc_shared_diag_reset(void);
adc_shared_diag_stats_t adc_shared_diag_get(void);
#endif

#if BISEN_HARVEST_ADC_DIAG_DEEP
#define ADC_DEEP_FULL_N 16u
#define ADC_DEEP_LP0_N  32u
#define ADC_DEEP_AVG1_N 64u
typedef struct {
    uint32_t trim_status;        // am_hal_adc_control() return code
    int32_t  trim_offset_x1e6;   // HAL offset correction x 1e6
    int32_t  trim_gain_x1e6;     // HAL gain correction x 1e6
    int32_t  trim_word3_x1e3;    // 4th float returned by the HAL x 1e3
    uint32_t cfg;                // ADC->CFG during the production pass
    uint32_t sl1cfg;             // ADC->SL1CFG during the production pass
    uint32_t full[ADC_DEEP_FULL_N];   // LPMODE1 AVG16, 12.6 fixed point
    uint16_t lp0[ADC_DEEP_LP0_N];     // LPMODE0 AVG16 integer codes
    uint16_t avg1[ADC_DEEP_AVG1_N];   // LPMODE1 AVG1 integer codes
} adc_shared_deep_t;

// Runs the three passes; leaves the ADC back in pixel mode. Failed
// conversions are stored as 0xFFFFFFFF / 0xFFFF.
void adc_shared_diag_deep(adc_shared_deep_t *out);
#endif

// Roughly what one adc_shared_read_supply() costs, in microseconds. This
// includes one discarded and three accepted averaged conversions plus mode
// switches.
uint32_t adc_shared_supply_cost_us(void);

// Reconstruct the imposed voltage BEFORE the divider. Despite the inherited
// function name, this is not a measurement of board VDD. Calibration is in
// trace_input.h; adc_shared_read_supply() always returns physical supply-pad ADC codes.
uint32_t adc_shared_supply_nominal_millivolts(uint32_t code);

// ---------------------------------------------------------------------------
// THE WAIT STATE -- watching the rail with the camera stopped
//
// Arm the hardware window comparator on the supply slot and put the ADC into
// repeating scan, so it samples on its own while the CPU does nothing. The
// comparison happens in hardware: instead of reading a value and comparing it
// in software on every poll, the caller checks a single status bit.
//
// THE WINDOW IS A BAND, AND THAT IS THE POINT. Arm it with the band the rail is
// currently sitting in: lower_code = the level at which a checkpoint becomes
// urgent, upper_code = the level at which work may restart. The hardware then
// signals an EXCURSION when the value leaves that band -- in EITHER direction.
//
// That is what makes this usable as the whole wait state. A single threshold
// would only answer "has it recovered?" and would miss the rail continuing to
// fall; watching the band means one armed comparator covers both outcomes, and
// a single real reading afterwards says which one happened.
//
// Codes are raw averaged ADC codes. Convert your millivolt thresholds with the
// same calibration you use for readings, then hand them here.
//
// Returns 0 if armed. Returns -1 if the hardware refused, in which case the
// caller MUST fall back to polling adc_shared_read_supply() -- see the note in
// power_policy.cc. Never assume arming succeeded.
int adc_shared_watch_arm(uint32_t lower_code, uint32_t upper_code);

// Has the rail left the armed band since arming? 0 = still inside, 1 = left.
// Reads a status bit; costs nothing and does not disturb the scan in progress.
//
// It does NOT say which way it went. Take one adc_shared_read_supply() afterwards
// and let the policy decide -- that is one real conversion for the whole wait,
// instead of one per poll.
//
// ⚠ POLLED, NOT INTERRUPT-DRIVEN. The ADC IRQ vector (am_adc_isr) belongs to
// bisen_adc.cc and its handler disables the IRQ whenever its own handle is null
// -- which it always is here, since this file does its own conversions. Taking
// the vector would mean editing that file. Polling one bit between sleeps costs
// far less than the reading it replaces, so the vector is left alone.
int adc_shared_watch_fired(void);

// The last raw code the watcher saw, for logging. Meaningful after arming.
uint32_t adc_shared_watch_last_code(void);

// Stop watching and return to ADC_MODE_PIXEL.
void adc_shared_watch_disarm(void);

// Is the ADC genuinely re-triggering on its own, or does the watcher need a
// software trigger per poll? Set during arming by observing whether a second
// scan completes unprompted. Reported so a serial trace shows which mechanism
// is actually running rather than which one was intended.
int adc_shared_watch_self_repeats(void);

#ifdef __cplusplus
}
#endif

#endif  // ADC_SHARED_H
