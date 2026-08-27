#ifndef ADC_SHARED_H
#define ADC_SHARED_H

#include <stdint.h>

// adc_shared.h -- the single owner of Apollo4 ADC instance 0.
//
// WHY THIS FILE EXISTS
// Apollo4 has ONE general-purpose ADC. Two pieces of code wanted to own it:
// sensor.c (photodiode readout on SE4/pin 15) and the BISen energy path (supply
// divider on SE3/pin 16). Each called am_hal_adc_initialize(0, ...) and the
// energy path also called am_hal_adc_deinitialize() after every read, which
// would have torn down the camera's ADC. The second initialize() fails, so the
// supply reading never worked at all.
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
    ADC_MODE_VCAP,        // slot 1 = SE3 divider,    LPMODE1 (53.7 us, lower power)
    ADC_MODE_WATCH,       // slot 1 + window comparator, repeating scan
} adc_mode_t;

// Initialise the peripheral once. Configures both pads, both slot templates,
// enters ADC_MODE_PIXEL, and pays the divider's one-time RC settle so that no
// later supply read has to. Call once, from sensor_init().
void adc_shared_init(void);

// Which mode is currently programmed.
adc_mode_t adc_shared_mode(void);

// Switch modes. Cheap (disable / configure / enable, a few microseconds) and
// idempotent -- asking for the mode already programmed does nothing.
void adc_shared_set_mode(adc_mode_t mode);

// One photodiode conversion. Requires ADC_MODE_PIXEL; the caller is sensor.c.
uint32_t adc_shared_read_pixel(void);

// One supply-divider conversion. Switches to ADC_MODE_VCAP, converts, and
// restores the previous mode. Returns 0 on success and writes the raw averaged
// code to *out_code; returns -1 and leaves *out_code alone on failure.
//
// The CALLER converts the code to millivolts. This file deliberately does not,
// so the bench calibration stays in bisen_config.h where its author put it.
int adc_shared_read_vcap(uint32_t *out_code);

// Roughly what one adc_shared_read_vcap() costs, in microseconds. Unlike the
// original path this does NOT include a 5 ms divider settle -- that is paid
// once, in adc_shared_init(), because the pad stays configured.
uint32_t adc_shared_vcap_cost_us(void);

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
// caller MUST fall back to polling adc_shared_read_vcap() -- see the note in
// power_policy.cc. Never assume arming succeeded.
int adc_shared_watch_arm(uint32_t lower_code, uint32_t upper_code);

// Has the rail left the armed band since arming? 0 = still inside, 1 = left.
// Reads a status bit; costs nothing and does not disturb the scan in progress.
//
// It does NOT say which way it went. Take one adc_shared_read_vcap() afterwards
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
