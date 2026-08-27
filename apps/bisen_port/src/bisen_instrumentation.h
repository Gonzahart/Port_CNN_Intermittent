#ifndef BISEN_INSTRUMENTATION_H
#define BISEN_INSTRUMENTATION_H

#include <stdint.h>

#include "bisen_config.h"

namespace bisen {

// Retains the three-bit state encoding used by the MSP430 BISen reference.
// Codes 5-7 are event markers; the scheduler's next state replaces them
// without inserting an instrumentation-only delay.
enum class InstrumentationCode : uint8_t {
    kSleep = 0u,
    kAdc = 1u,
    kTemperature = 2u,
    kCompute = 3u,
    kNonvolatileWrite = 4u,
    kCheckpointCommitted = 5u,
    kContextRestore = 6u,
    kBootOrError = 7u,
};

enum class InstrumentationStatus : uint8_t {
    kReady = 0u,
    kDisabled,
    kGpioConfigError,
};

#if BISEN_ENABLE_GPIO_INSTRUMENTATION
InstrumentationStatus instrumentation_init();
void instrumentation_set_code(InstrumentationCode code);
void instrumentation_set_state(State state);
#else
inline InstrumentationStatus instrumentation_init() {
    return InstrumentationStatus::kDisabled;
}

inline void instrumentation_set_code(InstrumentationCode) {}
inline void instrumentation_set_state(State) {}
#endif

}  // namespace bisen

#endif  // BISEN_INSTRUMENTATION_H
