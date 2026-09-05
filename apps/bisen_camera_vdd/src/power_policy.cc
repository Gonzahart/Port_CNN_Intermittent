// power_policy.cc -- see power_policy.h.
//
// Direct-threshold BISen policy glue for the externally driven camera.
#include "power_policy.h"

#include "am_mcu_apollo.h"
#include "am_util.h"
#include "bisen/bisen_energy.h"
#include "bisen/bisen_instrumentation.h"
#include "energy_source.h"
#include "adc_shared.h"
#include "workload.h"

// What the machine draws, from power.c's datasheet figures at 1.9 V. Only the
// simulated energy source uses these: a real one measures the consequence.
#ifndef PP_ACTIVE_UW
#define PP_ACTIVE_UW 7771u    /* 4090 uA x 1.9 V */
#endif
#ifndef PP_SLEEP_UW
#define PP_SLEEP_UW 49u       /* 26 uA x 1.9 V -- 158x less, which is the point */
#endif
#ifndef BISEN_CAMERA_VCAP_IGNORE_AT_MV
#define BISEN_CAMERA_VCAP_IGNORE_AT_MV 9000u
#endif
#ifndef BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES
#define BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES 32u
#endif
#if BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES < 1
#error "The high-VCAP discard filter requires at least one ADC attempt"
#endif
#if ES_SOURCE == ES_SOURCE_VCAP
static_assert(
    BISEN_CAMERA_VCAP_IGNORE_AT_MV >
        bisen::kBenchEnergyPolicy.chunk_1000_min_vcap_millivolts,
    "The high-sample discard cutoff must remain above every compute band");
#endif

namespace {

bisen::EnergyDecision s_decision;
uint32_t s_vcap_mv;
uint32_t s_raw_code;
bool     s_valid;
bool     s_failed_latched;
bool     s_high_retry_exhausted;
uint32_t s_high_samples_ignored;
uint32_t s_high_retry_exhaustions;


const char *band_name(bisen::EnergyBand b) {
    switch (b) {
        case bisen::EnergyBand::kChunk1000: return "chunk1000";
        case bisen::EnergyBand::kChunk500:  return "chunk500";
        case bisen::EnergyBand::kChunk100:  return "chunk100";
        case bisen::EnergyBand::kWait:      return "wait";
    }
    return "?";
}

}  // namespace

// --- state-bus self-check ---------------------------------------------------
#ifndef PP_BUS_SELFCHECK
#define PP_BUS_SELFCHECK 1
#endif

#if PP_BUS_SELFCHECK
static uint32_t s_bus_marks;
static uint32_t s_bus_bad;
static uint8_t  s_bus_last = 0xFFu;
// Per-code counts, not a sequence. A sequence buffer fills with whatever the
// hot loop alternates between -- here 1,3,1,3 -- and truncates before anything
// rare like a stop or a write ever appears. Counts show every code that fired.
static uint32_t s_bus_count[8];

static uint8_t bus_readback(void) {
    uint32_t v0 = 0u, v1 = 0u, v2 = 0u;
    (void)am_hal_gpio_state_read(bisen::kInstrumentationState0Gpio,
                                 AM_HAL_GPIO_OUTPUT_READ, &v0);
    (void)am_hal_gpio_state_read(bisen::kInstrumentationState1Gpio,
                                 AM_HAL_GPIO_OUTPUT_READ, &v1);
    (void)am_hal_gpio_state_read(bisen::kInstrumentationState2Gpio,
                                 AM_HAL_GPIO_OUTPUT_READ, &v2);
    return (uint8_t)((v0 & 1u) | ((v1 & 1u) << 1) | ((v2 & 1u) << 2));
}

static void bus_check(uint8_t intended) {
    s_bus_marks++;
    if (bus_readback() != intended) s_bus_bad++;
    s_bus_count[intended & 0x07u]++;
    s_bus_last = intended;
}
#else
static void bus_check(uint8_t) {}
#endif

extern "C" uint32_t pp_bus_marks(void) {
#if PP_BUS_SELFCHECK
    return s_bus_marks;
#else
    return 0u;
#endif
}
extern "C" uint32_t pp_bus_mismatches(void) {
#if PP_BUS_SELFCHECK
    return s_bus_bad;
#else
    return 0u;
#endif
}
extern "C" uint32_t pp_bus_last_code(void) {
#if PP_BUS_SELFCHECK
    return s_bus_last;
#else
    return 0u;
#endif
}
extern "C" uint32_t pp_bus_code_count(uint32_t code) {
#if PP_BUS_SELFCHECK
    return (code < 8u) ? s_bus_count[code] : 0u;
#else
    (void)code; return 0u;
#endif
}
extern "C" void pp_bus_reset(void) {
#if PP_BUS_SELFCHECK
    for (int i = 0; i < 8; i++) s_bus_count[i] = 0u;
    s_bus_last = 0xFFu;
#endif
}

extern "C" void pp_instr_init(void) {
    (void)bisen::instrumentation_init();
    // A source that cannot initialise reports valid = 0 for every read, so the
    // policy simply never fires rather than acting on a fabricated voltage.
    if (es_init() != 0) {
        am_util_stdio_printf(
            "POLICY: energy source '%s' unavailable -- no checkpoints will be"
            " triggered by energy in this build\n", es_source_name());
    }
}

extern "C" void pp_mark_boot(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kBootOrError);
    bus_check(7);
}
extern "C" void pp_mark_sleep(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kSleep);
    bus_check(0);
}
extern "C" void pp_mark_adc(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kAdc);
    bus_check(1);
}
extern "C" void pp_mark_camera(void) {
    // Code 2 was the MSP430 sensing state. In this application the physical
    // sensor is the 32x32 photodiode camera rather than die temperature.
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kSense);
    bus_check(2);
}
extern "C" void pp_mark_compute(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kCompute);
    bus_check(3);
}
extern "C" void pp_mark_nvm_write(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kNonvolatileWrite);
    bus_check(4);
}
extern "C" void pp_mark_committed(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kCheckpointCommitted);
    bus_check(5);
}
extern "C" void pp_mark_restore(void) {
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kContextRestore);
    bus_check(6);
}

extern "C" void pp_sample(void) {
    pp_mark_adc();

    es_reading_t r = {};
    for (uint32_t attempt = 0u;
         attempt < BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES; ++attempt) {
        r = {};
        es_read(&r);
        if (!r.discard_and_retry) {
            s_high_retry_exhausted = false;
            break;
        }

        ++s_high_samples_ignored;
        // Retain the rejected measurement only for an explicit fallback log.
        // It is never passed to choose_energy_policy().
        s_vcap_mv = r.millivolts;
        s_raw_code = r.raw_code;
        if (attempt + 1u == BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES) {
            ++s_high_retry_exhaustions;
            s_high_retry_exhausted = true;
            s_valid = false;
            return;
        }
    }

    if (!r.valid) {
        s_raw_code = 0u;
        s_valid = false;
        return;
    }

    // A source with no voltage -- a comparator or PMU interrupt -- cannot be
    // banded, because there is nothing to compare against a threshold. Its
    // level IS the decision, so map it straight onto the fields the rest of
    // this file reads and skip the band policy entirely.
    if (!r.has_millivolts) {
        s_decision = {};
        s_decision.band = bisen::EnergyBand::kWait;
        s_decision.chunk_pixels = 0u;
        s_decision.restore_allowed = (r.level == ES_LEVEL_OK);
        s_decision.compute_allowed = (r.level == ES_LEVEL_OK);
        s_decision.below_sleep_floor = (r.level == ES_LEVEL_CRITICAL);
        s_vcap_mv = 0u;
        s_raw_code = r.raw_code;
        s_valid = true;
        return;
    }

    bisen::EnergyObservation obs = {};
    obs.previous_band = bisen::EnergyBand::kWait;  // ignored: no hysteresis
    obs.valid = true;
    obs.simulated = r.simulated != 0;
    obs.corrected_adc_code = r.raw_code;
    obs.policy_vcap_millivolts = r.millivolts;
    obs.vcap_volts = static_cast<float>(r.millivolts) / 1000.0f;
    obs.nominal_vcap_volts = obs.vcap_volts;

    if (bisen::choose_energy_policy(obs, &s_decision) !=
        bisen::EnergyStatus::kReady) {
        s_valid = false;
        return;
    }
    s_vcap_mv = obs.policy_vcap_millivolts;
    s_raw_code = r.raw_code;
    s_valid = true;
}

extern "C" int pp_should_checkpoint(void) {
    if (!s_valid) return 0;
    // job_complete is left false: wl_dirty() already reports 0 once a phase has
    // finished, so completion cannot request a write. Passing both would be
    // saying the same thing twice.
    const bisen::PreSleepCheckpointAction action =
        bisen::choose_pre_sleep_checkpoint_action(
            s_decision.compute_allowed, wl_dirty() != 0, false,
            s_failed_latched);
    // main.c has one decision: when the next useful state is unaffordable and
    // progress changed, checkpoint before sleeping. There is no second
    // safe-write voltage floor here.
    return action == bisen::PreSleepCheckpointAction::kWrite;
}

extern "C" uint32_t pp_chunk_units(void) {
    return s_valid ? s_decision.chunk_pixels : 0u;
}

extern "C" int pp_compute_allowed(void) {
    return s_valid && s_decision.compute_allowed;
}

// Invert the active board conversion so the ADC window comparator and software
// policy use exactly the same voltage-to-code mapping.
#if ES_SOURCE == ES_SOURCE_VCAP
static uint32_t code_from_millivolts(uint32_t mv) {
    const int64_t nv =
        static_cast<int64_t>((uint64_t)mv * bisen::kNanovoltsPerMillivolt);
    const int64_t numerator = nv - bisen::kVcapCalibrationOffsetNanovolts;
    if (numerator <= 0) return 0u;
    return static_cast<uint32_t>(
        static_cast<uint64_t>(numerator) /
        bisen::kVcapCalibrationNanovoltsPerCode);
}
#endif

static int s_wait_armed;

// The band is the CALLER's, passed in, not read from a threshold table here.
// Which thresholds matter is policy, and policy does not belong in the layer
// that owns the peripheral -- that is the whole point of the split.
extern "C" int pp_wait_arm(uint32_t low_mv, uint32_t high_mv) {
#if ES_SOURCE == ES_SOURCE_VCAP
    if (low_mv >= high_mv) return 0;      // not a band
    s_wait_armed = (adc_shared_watch_arm(code_from_millivolts(low_mv),
                                         code_from_millivolts(high_mv)) == 0);
    return s_wait_armed;
#else
    (void)low_mv; (void)high_mv;
    // Simulated and pin sources have no comparator to arm. Saying so plainly is
    // what keeps the caller on its polling path instead of waiting forever.
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// The wl_* doors. See workload.h for what they promise; these are thin on
// purpose. They live here rather than in workload.c because the millivolt
// conversion needs the C++ calibration in bisen_config.h.

extern "C" int wl_supply_mv(uint32_t *out_mv) {
#if ES_SOURCE == ES_SOURCE_VCAP
    if (out_mv == 0) return -1;
    uint32_t code;
    if (adc_shared_read_vcap(&code) != 0) return -1;
    *out_mv = bisen::calibrated_vcap_millivolts_from_code(code);
    return 0;
#elif ES_SOURCE == ES_SOURCE_BATT
    if (out_mv == 0) return -1;
    uint32_t code;
    if (adc_shared_read_supply(&code) != 0) return -1;
    *out_mv = adc_shared_supply_nominal_millivolts(code);
    return 0;
#else
    // Not "0 mV" -- there is no supply source to read, and a caller must be
    // able to tell that apart from a rail that has collapsed.
    (void)out_mv;
    return -1;
#endif
}

extern "C" uint32_t bisen_checkpoint_trigger_mv(void) {
    return bisen::kBenchEnergyPolicy.chunk_100_min_vcap_millivolts;
}
extern "C" uint32_t bisen_resume_mv(void) {
    return bisen::kBenchEnergyPolicy.resume_vcap_millivolts;
}

extern "C" int wl_wait_arm(uint32_t low_mv, uint32_t high_mv) {
    return pp_wait_arm(low_mv, high_mv);
}

extern "C" int wl_wait_changed(void) {
    // pp_wait_left_band() answers 1 when nothing is armed, which is right for
    // the self-driving loop -- it means "stop waiting on me and go sample".
    // For an external caller that would be a lie, so unarmed reports 0 here and
    // wl_wait_arm()'s return value stays the only thing that decides which wait
    // a caller uses.
    return s_wait_armed ? pp_wait_left_band() : 0;
}

extern "C" void wl_wait_disarm(void) { pp_wait_disarm(); }

extern "C" int pp_wait_left_band(void) {
#if ES_SOURCE == ES_SOURCE_VCAP
    return s_wait_armed ? adc_shared_watch_fired() : 1;
#else
    return 1;
#endif
}

extern "C" void pp_wait_disarm(void) {
#if ES_SOURCE == ES_SOURCE_VCAP
    if (s_wait_armed) { adc_shared_watch_disarm(); s_wait_armed = 0; }
#endif
}

extern "C" int pp_wait_self_repeats(void) {
#if ES_SOURCE == ES_SOURCE_VCAP
    return s_wait_armed ? adc_shared_watch_self_repeats() : 0;
#else
    return 0;
#endif
}

extern "C" void pp_enter_wait(void) {
    pp_mark_sleep();
    es_set_load_uw(PP_SLEEP_UW);
}

extern "C" void pp_leave_wait(void) {
    es_set_load_uw(PP_ACTIVE_UW);
    pp_mark_compute();
}

extern "C" int pp_restore_allowed(void) {
    return s_valid && s_decision.restore_allowed;
}

extern "C" void pp_note_checkpoint_failed(void) { s_failed_latched = true; }

extern "C" uint32_t pp_vcap_mv(void) { return s_vcap_mv; }
extern "C" uint32_t pp_adc_code(void) { return s_raw_code; }

extern "C" const char *pp_band_name(void) {
    if (s_valid) return band_name(s_decision.band);
    return s_high_retry_exhausted ? "high-ignore" : "unsampled";
}

extern "C" void pp_high_sample_filter_reset(void) {
    s_high_samples_ignored = 0u;
    s_high_retry_exhaustions = 0u;
}

extern "C" uint32_t pp_high_samples_ignored(void) {
    return s_high_samples_ignored;
}

extern "C" uint32_t pp_high_retry_exhaustions(void) {
    return s_high_retry_exhaustions;
}

extern "C" void pp_report(void) {
    if (!s_valid) {
        am_util_stdio_printf("POLICY: no valid supply reading\n");
        return;
    }
    wl_state_t st;
    wl_state(&st);
    am_util_stdio_printf(
        "POLICY: code=%u nominal_supply=%u mV [%s] band=%s chunk=%u restore=%u compute=%u"
        " checkpoint_now=%u | workload=%s dirty=%u %u B ~%u us\n",
        (unsigned)s_raw_code, (unsigned)s_vcap_mv, es_source_name(),
        pp_band_name(), (unsigned)s_decision.chunk_pixels,
        s_decision.restore_allowed ? 1u : 0u,
        s_decision.compute_allowed ? 1u : 0u,
        pp_should_checkpoint() ? 1u : 0u,
        wl_phase_name(st.phase), (unsigned)st.dirty,
        (unsigned)st.payload_bytes, (unsigned)st.write_us);
}
