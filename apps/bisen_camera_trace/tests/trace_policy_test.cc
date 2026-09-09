#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "trace_input.h"
#include "bisen/bisen_energy.h"

static void check(uint32_t uv) {
    bisen::EnergyObservation obs = {};
    obs.valid = true;
    obs.simulated = true;
    obs.corrected_adc_code = trace_policy_code(uv);
    obs.policy_vcap_millivolts = uv / 1000;
    bisen::EnergyDecision d = {};
    assert(bisen::choose_energy_policy(obs, &d) == bisen::EnergyStatus::kReady);
    const unsigned budget = uv >= 2200000 ? 1000 : uv >= 2100000 ? 500 :
                            uv >= 2000000 ? 100 : 0;
    assert(d.chunk_pixels == budget);
    assert(d.compute_allowed == (uv >= 2000000));
    assert(d.restore_allowed == (uv >= 2100000));
    assert(d.below_sleep_floor == (uv < 1900000));
    // Direction must have no effect on a new decision.
    obs.previous_band = bisen::EnergyBand::kChunk1000;
    bisen::EnergyDecision reverse = {};
    bisen::choose_energy_policy(obs, &reverse);
    assert(reverse.band == d.band);
    assert(reverse.restore_allowed == d.restore_allowed);
}

int main() {
    for (uint32_t anchor : {1900000u, 2000000u, 2100000u, 2200000u}) {
        check(anchor - 1); check(anchor); check(anchor + 1);
    }
    uint32_t previous_uv = 0, previous_code = 0;
    for (uint32_t raw = 0; raw < 4096; ++raw) {
        const uint32_t uv = trace_input_microvolts(raw);
        assert(uv >= previous_uv);
        const uint32_t code = trace_policy_code(uv);
        assert(code >= previous_code && code <= 4095);
        check(uv);
        previous_uv = uv; previous_code = code;
    }
    assert(trace_input_microvolts(BISEN_TRACE_CAL_LOW_CODE) == BISEN_TRACE_CAL_LOW_UV);
    assert(trace_input_microvolts(BISEN_TRACE_CAL_HIGH_CODE) == BISEN_TRACE_CAL_HIGH_UV);
    using bisen::choose_pre_sleep_checkpoint_action;
    using bisen::PreSleepCheckpointAction;
    assert(choose_pre_sleep_checkpoint_action(false, true, false, false) == PreSleepCheckpointAction::kWrite);
    assert(choose_pre_sleep_checkpoint_action(false, false, false, false) == PreSleepCheckpointAction::kNone);
    assert(choose_pre_sleep_checkpoint_action(true, true, false, false) == PreSleepCheckpointAction::kNone);
    assert(choose_pre_sleep_checkpoint_action(false, true, true, false) == PreSleepCheckpointAction::kNone);
    assert(choose_pre_sleep_checkpoint_action(false, true, false, true) == PreSleepCheckpointAction::kNone);
    puts("PASS: 4096 ADC inputs, threshold edges, direction independence, calibration and checkpoint gating");
}
