#ifndef BISEN_TEMPERATURE_H
#define BISEN_TEMPERATURE_H

#include <stdint.h>

namespace bisen {

enum class TemperatureStatus : uint8_t {
    kReady = 0,
    kInvalidArgument,
    kAdcInitializeError,
    kAdcPowerError,
    kAdcConfigureError,
    kAdcSlotConfigureError,
    kAdcEnableError,
    kAdcPrimeError,
    kAdcSampleError,
    kCelsiusConversionError,
};

struct TemperatureObservation {
    bool valid;
    uint32_t corrected_adc_code;
    float sensor_volts;
    float celsius;
};

TemperatureStatus measure_die_temperature(TemperatureObservation *observation);

}  // namespace bisen

#endif  // BISEN_TEMPERATURE_H
