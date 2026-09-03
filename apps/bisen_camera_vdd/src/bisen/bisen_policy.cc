#include "bisen_energy.h"

namespace bisen {
namespace {

EnergyBand classify_energy(uint32_t vcap_millivolts) {
    if (vcap_millivolts >=
        kBenchEnergyPolicy.chunk_1000_min_vcap_millivolts) {
        return EnergyBand::kChunk1000;
    }
    if (vcap_millivolts >=
        kBenchEnergyPolicy.chunk_500_min_vcap_millivolts) {
        return EnergyBand::kChunk500;
    }
    if (vcap_millivolts >=
        kBenchEnergyPolicy.chunk_100_min_vcap_millivolts) {
        return EnergyBand::kChunk100;
    }
    return EnergyBand::kWait;
}

}  // namespace

EnergyStatus choose_energy_policy(const EnergyObservation &observation,
                                  EnergyDecision *decision) {
    if (decision == nullptr || !observation.valid) {
        return EnergyStatus::kHardwareUnresolved;
    }

    *decision = {};
    const uint32_t vcap = observation.policy_vcap_millivolts;

    // Match the MSP430 reference and the validated bisen_port scheduler:
    // select the current action directly from the latest sample. There is no
    // previous-band retention and no hysteresis.
    decision->band = classify_energy(vcap);
    switch (decision->band) {
        case EnergyBand::kChunk1000:
            decision->chunk_pixels = kBenchEnergyPolicy.chunk_1000_pixels;
            break;
        case EnergyBand::kChunk500:
            decision->chunk_pixels = kBenchEnergyPolicy.chunk_500_pixels;
            break;
        case EnergyBand::kChunk100:
            decision->chunk_pixels = kBenchEnergyPolicy.chunk_100_pixels;
            break;
        case EnergyBand::kWait:
            decision->chunk_pixels = 0u;
            break;
    }
    decision->restore_allowed =
        vcap >= kBenchEnergyPolicy.resume_vcap_millivolts;
    decision->temperature_allowed = decision->restore_allowed;
    decision->compute_allowed =
        BISEN_ENABLE_COMPUTE_WORKLOAD != 0 &&
        vcap >= kBenchEnergyPolicy.chunk_100_min_vcap_millivolts &&
        decision->chunk_pixels != 0u;
    decision->below_sleep_floor =
        vcap < kBenchEnergyPolicy.sleep_below_vcap_millivolts;
    return EnergyStatus::kReady;
}

PreSleepCheckpointAction choose_pre_sleep_checkpoint_action(
    bool next_useful_state_allowed, bool context_dirty, bool job_complete,
    bool failure_latched) {
    // Match main.c: once the next useful state is unaffordable, persist dirty,
    // incomplete progress before the energy-retry sleep. Completed work is not
    // a recovery checkpoint and never requests an MRAM write.
    if (next_useful_state_allowed || !context_dirty || job_complete ||
        failure_latched) {
        return PreSleepCheckpointAction::kNone;
    }
    return PreSleepCheckpointAction::kWrite;
}

}  // namespace bisen
