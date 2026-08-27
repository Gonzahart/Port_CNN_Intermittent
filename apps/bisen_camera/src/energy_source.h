// ===========================================================================
// NOT A REPLACEMENT FOR bisen_energy.cc, AND NOT PART OF THE API.
//
// Measuring the supply is the BISen port's job and bisen_energy.cc does it.
// This file is a SELECTOR that picks where a reading comes from, and one of the
// things it can select is exactly that code -- ES_SOURCE_VCAP calls
// bisen::measure_energy() unchanged.
//
// The camera and divider share Apollo4 ADC0 through adc_shared.c. The physical
// AMAP4PEVB build uses the divider on J9.8/GPIO16/ADCSE3; the selector remains
// useful for safe no-divider diagnostics and future PMU/comparator sources.
//
// bisen_policy.cc still needs a millivolt reading from somewhere in order to
// run at all, so this supplies one. In the externally-driven build the reading
// feeds NOTHING but the POLICY log line -- the scheduler owns the decision and
// samples however it likes. Deleting this file would cost that log line and the
// ADC code on the state bus, and nothing else.
//
// The four sources are here rather than one boolean because they are not
// interchangeable in shape: a PMU or comparator asserting a GPIO reports no
// voltage at all, which a millivolt-only interface cannot express -- and that
// is what the BISen paper describes the reference hardware doing.
// ===========================================================================

#ifndef ENERGY_SOURCE_H
#define ENERGY_SOURCE_H

#include <stdint.h>

// energy_source.h -- where the "power is about to fail" signal comes from.
//
// The checkpoint machinery does not care what decided to save; it only cares
// that something did. This is that something, behind one interface, so the
// source can change without touching the policy, the workload or the storage.
//
// WHY AN ABSTRACTION AND NOT JUST AN ADC CALL
// The candidate sources are not interchangeable in shape:
//
//   simulated ramp     millivolts, free, no hardware, proves nothing electrical
//   VCAP divider+ADC   millivolts, shared ADC, external capacitor/divider
//   internal BATT      millivolts, cheap, but reads the REGULATED rail, so it
//                      is flat until the regulator drops out -- late warning
//   PMU / comparator   NO voltage at all, just an edge saying "now" -- which is
//                      what BISen's own hardware does ("two external interrupts
//                      are triggered by intelligent power management")
//
// A voltage-only interface cannot express the last one, and a "save now" only
// interface throws away the band information the BISen policy needs. So a
// reading carries both, and says which of the two is meaningful.
//
// It also carries two things nothing currently tracks:
//
//   read_cost_us   what asking costs, so callers pace their own polling
//                  instead of a hardcoded interval that silently breaks when
//                  the source changes
//   warning_us     how long the supply is expected to last, so a caller can
//                  ask the question that actually matters: do I have time to
//                  finish this write?

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ES_LEVEL_UNKNOWN = 0,   // no usable reading
    ES_LEVEL_OK,            // energy to keep working
    ES_LEVEL_LOW,           // save now, while a write can still complete
    ES_LEVEL_CRITICAL,      // stop; a write may no longer finish
} es_level_t;

typedef struct {
    uint8_t    valid;             // 1 => this reading may drive the policy
    uint8_t    discard_and_retry; // 1 => measured, but caller must sample again
    uint8_t    has_millivolts;    // 1 => `millivolts` is real and may be banded
    uint8_t    simulated;         // 1 => not measured from hardware. Say so in logs.
    uint32_t   millivolts;
    es_level_t level;             // always meaningful, even without a voltage
    uint32_t   warning_us;        // expected time before collapse; 0 = unknown
    uint32_t   raw_code;          // ADC code when there is one, for diagnostics
} es_reading_t;

// Which source this build uses. Selected at compile time because each one
// implies different hardware, and a runtime choice would mean carrying code for
// hardware that is not fitted.
#define ES_SOURCE_SIM   0   // synthetic ramp. Default: needs nothing, proves nothing.
#define ES_SOURCE_VCAP  1   // external divider through the ADC (his bisen_energy)
#define ES_SOURCE_BATT  2   // internal AM_HAL_ADC_SLOT_CHSEL_BATT, no external parts
#define ES_SOURCE_PIN   3   // a PMU / comparator asserting a GPIO

#ifndef ES_SOURCE
#define ES_SOURCE ES_SOURCE_SIM
#endif

// Prepare the source. Returns 0 on success, -1 if the hardware it needs is not
// present or not yet wired -- in which case es_read() reports valid = 0 and the
// caller should fall back to whatever it does with no energy information,
// rather than assuming everything is fine.
int es_init(void);

// Take a reading. Cost varies by source -- see es_read_cost_us().
void es_read(es_reading_t *out);

// Tell the model what the machine is currently drawing, in microwatts.
//
// Only the simulated source uses it; a real source measures the consequence
// rather than predicting it, and ignores this. It exists because BISen's Stop
// state is defined by consuming far less than the active state -- if a model
// drains at the same rate whether computing or asleep, waiting can never
// recover anything and the Th_safe mechanism cannot be exercised at all.
void es_set_load_uw(uint32_t microwatts);

// What one es_read() costs, in microseconds. 0 for sources that are free
// (simulated, or an interrupt flag that is already set). Callers use this to
// choose their own polling interval: a 3.6 s scan can afford a 5 ms read every
// 200 ms, a 23 ms inference cannot afford one at all.
uint32_t es_read_cost_us(void);

// Human-readable source name, for logs. Says whether it is simulated.
const char *es_source_name(void);

// Can a write of `bytes_us` microseconds still finish, given the last reading?
// Returns 1 when there is time, 0 when there is not, and 1 when the source
// cannot say (warning_us == 0) -- an unknown budget must not silently suppress
// checkpoints, since the alternative is losing the work outright.
int es_write_fits(uint32_t write_us);

#ifdef __cplusplus
}
#endif

#endif  // ENERGY_SOURCE_H
