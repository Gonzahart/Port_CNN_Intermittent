#ifndef ENERGY_SOURCE_H
#define ENERGY_SOURCE_H

#include <stdint.h>

// This app samples the physical capacitor that powers the MP1584EN/board.

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
    uint32_t   raw_code;          // virtual direct-VDD policy code in this app
} es_reading_t;

#define ES_SOURCE_SIM   0
#define ES_SOURCE_VCAP  1
#define ES_SOURCE_BATT  2
#define ES_SOURCE_PIN   3
#define ES_SOURCE_TRACE 4
#define ES_SOURCE_HARVEST 5
#ifndef ES_SOURCE
#define ES_SOURCE ES_SOURCE_HARVEST
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
