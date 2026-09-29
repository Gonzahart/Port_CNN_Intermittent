#ifndef BISEN_ENERGY_H
#define BISEN_ENERGY_H

#include "bisen_config.h"

namespace bisen {

enum class EnergyStatus : uint8_t {
    kReady = 0,
    kHardwareUnresolved,
    kInvalidArgument,
    kGpioConfigError,
    kAdcInitializeError,
    kAdcPowerError,
    kAdcConfigureError,
    kAdcSlotConfigureError,
    kAdcEnableError,
    kAdcSampleError,
    kAdcOutOfValidationRange,
};

struct EnergyObservation {
    bool valid;
    bool simulated;
    uint32_t priming_adc_code;
    uint32_t corrected_adc_code;
    float adc_volts;
    float nominal_vcap_volts;
    uint32_t policy_vcap_millivolts;
    float vcap_volts;
    EnergyBand previous_band;
};

struct EnergyDecision {
    EnergyBand band;
    uint32_t chunk_pixels;
    bool restore_allowed;
    bool temperature_allowed;
    bool compute_allowed;
    bool below_sleep_floor;
};

enum class PreSleepCheckpointAction : uint8_t {
    kNone = 0,
    kWrite,
};

EnergyStatus measure_energy(EnergyObservation *observation);
EnergyStatus choose_energy_policy(
    const EnergyObservation &observation, EnergyDecision *decision);
PreSleepCheckpointAction choose_pre_sleep_checkpoint_action(
    bool next_useful_state_allowed, bool context_dirty, bool job_complete,
    bool failure_latched);

}  // namespace bisen

#endif  // BISEN_ENERGY_H
