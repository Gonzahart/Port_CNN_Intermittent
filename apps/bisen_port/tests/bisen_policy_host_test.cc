#include <assert.h>
#include <stdio.h>

#include "bisen_energy.h"

namespace {

bisen::EnergyDecision decide(uint32_t vcap_millivolts,
                             bisen::EnergyBand previous) {
    bisen::EnergyObservation observation = {};
    observation.valid = true;
    observation.simulated = true;
    observation.policy_vcap_millivolts = vcap_millivolts;
    observation.vcap_volts =
        static_cast<float>(vcap_millivolts) / 1000.0f;
    observation.nominal_vcap_volts = observation.vcap_volts;
    observation.previous_band = previous;
    bisen::EnergyDecision decision = {};
    assert(bisen::choose_energy_policy(observation, &decision) ==
           bisen::EnergyStatus::kReady);
    return decision;
}

}  // namespace

int main() {
    using bisen::EnergyBand;

    // The fixed-point bench calibration is deterministic and distinct from
    // the nominal divider/reference safety estimate.
    assert(bisen::calibrated_vcap_millivolts_from_code(966u) == 5700u);
    assert(bisen::calibrated_vcap_millivolts_from_code(1036u) == 6098u);
    assert(bisen::calibrated_vcap_millivolts_from_code(1549u) == 9012u);

    assert(decide(9000u, EnergyBand::kWait).band ==
           EnergyBand::kChunk1000);
    assert(decide(7500u, EnergyBand::kWait).chunk_pixels == 500u);
    assert(decide(7000u, EnergyBand::kWait).chunk_pixels == 500u);
    assert(decide(6200u, EnergyBand::kWait).chunk_pixels == 100u);
    assert(decide(5800u, EnergyBand::kWait).band == EnergyBand::kWait);

    // The MSP430 reference uses direct comparisons, so the result must be
    // independent of the previous band at every exact boundary.
    assert(decide(8400u, EnergyBand::kChunk1000).band ==
           EnergyBand::kChunk500);
    assert(decide(8200u, EnergyBand::kChunk1000).band ==
           EnergyBand::kChunk500);
    assert(decide(6300u, EnergyBand::kChunk500).band ==
           EnergyBand::kChunk100);
    assert(decide(6100u, EnergyBand::kChunk500).band ==
           EnergyBand::kChunk100);
    assert(decide(6500u, EnergyBand::kChunk100).band ==
           EnergyBand::kChunk500);
    assert(decide(6700u, EnergyBand::kChunk100).band ==
           EnergyBand::kChunk500);
    assert(decide(6000u, EnergyBand::kWait).band ==
           EnergyBand::kChunk100);
    assert(decide(8500u, EnergyBand::kWait).band ==
           EnergyBand::kChunk1000);
    assert(decide(6400u, EnergyBand::kChunk1000).band ==
           EnergyBand::kChunk500);
    assert(decide(5900u, EnergyBand::kChunk500).band ==
           EnergyBand::kChunk100);
    assert(decide(5899u, EnergyBand::kChunk100).band ==
           EnergyBand::kWait);

    const bisen::EnergyDecision qualified =
        decide(6200u, EnergyBand::kWait);
    assert(qualified.restore_allowed);
    assert(qualified.temperature_allowed);
    assert(qualified.compute_allowed);
    assert(!qualified.below_sleep_floor);
    assert(bisen::choose_pre_sleep_checkpoint_action(
               qualified.compute_allowed, true, false, false) ==
           bisen::PreSleepCheckpointAction::kNone);

    const bisen::EnergyDecision no_new_work =
        decide(5800u, EnergyBand::kWait);
    assert(!no_new_work.restore_allowed);
    assert(!no_new_work.temperature_allowed);
    assert(!no_new_work.compute_allowed);
    assert(!no_new_work.below_sleep_floor);
    const bisen::EnergyDecision falling_no_new_work =
        decide(5800u, EnergyBand::kChunk100);
    assert(falling_no_new_work.band == EnergyBand::kWait);
    assert(!falling_no_new_work.compute_allowed);

    // The MSP430-aligned checkpoint decision has no independent voltage
    // threshold. Dirty, incomplete progress is saved immediately before an
    // energy-driven sleep when no next useful state can run.
    assert(bisen::choose_pre_sleep_checkpoint_action(
               falling_no_new_work.compute_allowed, true, false, false) ==
           bisen::PreSleepCheckpointAction::kWrite);
    assert(bisen::choose_pre_sleep_checkpoint_action(
               false, false, false, false) ==
           bisen::PreSleepCheckpointAction::kNone);
    assert(bisen::choose_pre_sleep_checkpoint_action(
               false, true, true, false) ==
           bisen::PreSleepCheckpointAction::kNone);
    assert(bisen::choose_pre_sleep_checkpoint_action(
               false, true, false, true) ==
           bisen::PreSleepCheckpointAction::kNone);
    assert(!decide(6099u, EnergyBand::kWait).restore_allowed);
    assert(decide(6100u, EnergyBand::kWait).restore_allowed);

    const bisen::EnergyDecision sleep =
        decide(5499u, EnergyBand::kWait);
    assert(sleep.below_sleep_floor);
    assert(!decide(5500u, EnergyBand::kWait).below_sleep_floor);

    puts("bisen_policy_host_test: PASS");
    return 0;
}
