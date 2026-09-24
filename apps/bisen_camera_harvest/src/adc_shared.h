#ifndef ADC_SHARED_H
#define ADC_SHARED_H

#include <stdint.h>

// adc_shared.h -- the single owner of Apollo4 ADC instance 0.
//
// WHY THIS FILE EXISTS
// Apollo4 has ONE general-purpose ADC. Two pieces of code wanted to own it:
// sensor.c (photodiode readout on SE4/pin 15) and the BISen energy path (supply
// external trace input (GPIO16/ADCSE3). Both must share ADC0; a
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
    ADC_MODE_SUPPLY,      // slot 1 = external ADCSE3, LPMODE1 (53.7 us, lower power)
    ADC_MODE_WATCH,       // slot 1 + window comparator, repeating scan
} adc_mode_t;

// Initialise the peripheral once. Configures the photodiode pad and both slot
// templates, then enters ADC_MODE_PIXEL. Configures GPIO16/ADCSE3 once and
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

// Roughly what one adc_shared_read_supply() costs, in microseconds. This
// includes one discarded and three accepted averaged conversions plus mode
// switches.
uint32_t adc_shared_supply_cost_us(void);

// Reconstruct the imposed voltage BEFORE the divider. Despite the inherited
// function name, this is not a measurement of board VDD. Calibration is in
// trace_input.h; adc_shared_read_supply() always returns physical ADCSE3 codes.
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
