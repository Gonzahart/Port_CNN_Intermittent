#include "bisen_checkpoint.h"
#include "bisen_adc.h"
#include "bisen_compute.h"
#include "bisen_config.h"
#include "bisen_energy.h"
#include "bisen_instrumentation.h"
#include "bisen_log.h"
#include "bisen_power.h"
#include "bisen_runtime.h"
#include "bisen_temperature.h"

#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "ns_ambiqsuite_harness.h"
#include "ns_core.h"
#include "ns_peripherals_button.h"
#include "ns_peripherals_power.h"

extern "C" uint8_t __bisen_retained_start__[];
extern "C" uint8_t __bisen_retained_end__[];

#if BISEN_ENABLE_DEBUG_LOGGING
#define BISEN_LOG(...) ns_lp_printf(__VA_ARGS__)
#else
#define BISEN_LOG(...) do { } while (0)
#endif

namespace {

#if BISEN_ENABLE_DEBUG_LOGGING
const char *state_name(bisen::State state) {
    switch (state) {
        case bisen::State::kWakeRestore: return "Wake / qualification";
        case bisen::State::kAdc: return "VCAP ADC";
        case bisen::State::kTemperatureSense: return "Die temperature";
        case bisen::State::kCompute: return "Resumable Sobel";
        case bisen::State::kNonvolatileCheckpoint: return "MRAM checkpoint";
        case bisen::State::kSleepLowPowerWait: return "RTC deep sleep";
    }
    return "Unknown";
}

const char *energy_name(bisen::EnergyBand band) {
    switch (band) {
        case bisen::EnergyBand::kChunk1000: return "1000px";
        case bisen::EnergyBand::kChunk500: return "500px";
        case bisen::EnergyBand::kChunk100: return "100px";
        case bisen::EnergyBand::kWait: return "wait";
    }
    return "unknown";
}
#endif

enum class RunMode : uint8_t {
    kBounded = 0,
    kContinuous,
};

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
volatile int g_continuous_mode_requested = 0;
bool g_continuous_mode_button_armed = false;
constexpr uint32_t kModeButtonDebounceUs = 20000u;
#endif

struct Scheduler {
    bisen::State state;
    bisen::BisenRuntimeContext *runtime;
    bisen::Context &context;
    bisen::EnergyDecision decision;
    bool restore_attempted;
    uint32_t restored_checkpoint_sequence;
    bool interrupted_checkpoint_detected;
    bool hard_failure;
    bool bounded_park_allows_continuous;
    bool post_compute_measurement;
    uint32_t last_progress_rtc_wake;
    RunMode run_mode;
};

void append_event(Scheduler *scheduler, bisen::BisenLogEvent event,
                  uint16_t flags = 0u) {
    bisen::bisen_log_append(event, flags, bisen::rtc_wake_count(),
                            *scheduler->runtime);
}

void set_state(Scheduler *scheduler, bisen::State state) {
    scheduler->state = state;
    bisen::instrumentation_set_state(state);
    BISEN_LOG("BISen state: %s\n", state_name(state));
}

#if BISEN_ENABLE_ADC_DIAGNOSTIC
void log_adc_debug_snapshot(const char *label, uint32_t sample_index) {
    const bisen::AdcDebugSnapshot snapshot = bisen::adc_debug_snapshot();
    BISEN_LOG("BISen ADC regs %s #%lu pre: CFG=%08lx STAT=%08lx SL0=%08lx FIFO=%08lx INTEN=%08lx INTSTAT=%08lx\n",
              label,
              static_cast<unsigned long>(sample_index),
              static_cast<unsigned long>(snapshot.cfg_before_trigger),
              static_cast<unsigned long>(snapshot.stat_before_trigger),
              static_cast<unsigned long>(snapshot.slot0_before_trigger),
              static_cast<unsigned long>(snapshot.fifo_before_trigger),
              static_cast<unsigned long>(snapshot.inten_before_trigger),
              static_cast<unsigned long>(snapshot.intstat_before_trigger));
    BISEN_LOG("BISen ADC regs %s #%lu post: CFG=%08lx STAT=%08lx SL0=%08lx FIFO=%08lx INTEN=%08lx INTSTAT=%08lx IRQ_LAST=%08lx IRQ_ALL=%08lx attempts=%lu empty=%lu PWR_EN=%08lx PWR_STAT=%08lx ADC_PWR=%08lx ADC_DLY=%08lx\n",
              label,
              static_cast<unsigned long>(sample_index),
              static_cast<unsigned long>(snapshot.cfg_after_interrupt),
              static_cast<unsigned long>(snapshot.stat_after_interrupt),
              static_cast<unsigned long>(snapshot.slot0_after_interrupt),
              static_cast<unsigned long>(snapshot.fifo_after_interrupt),
              static_cast<unsigned long>(snapshot.inten_after_interrupt),
              static_cast<unsigned long>(snapshot.intstat_after_interrupt),
              static_cast<unsigned long>(snapshot.interrupt_status),
              static_cast<unsigned long>(
                  snapshot.accumulated_interrupt_status),
              static_cast<unsigned long>(snapshot.trigger_attempts),
              static_cast<unsigned long>(snapshot.empty_scan_count),
              static_cast<unsigned long>(snapshot.devpwren),
              static_cast<unsigned long>(snapshot.devpwrstatus),
              static_cast<unsigned long>(snapshot.adcpwrctrl),
              static_cast<unsigned long>(snapshot.adcpwrdly));
}

[[noreturn]] void run_adc_diagnostic() {
    constexpr uint32_t kDiagnosticSamples = 16u;
    BISEN_LOG("BISen ADC diagnostic v6: 16-sample LPMODE1 repeatability, MRAM=FORCED-OFF compute=off RTC=off GPIO15/ADCSE4\n");

    am_hal_gpio_pincfg_t initial_pin_config = {};
    const uint32_t initial_pin_status = am_hal_gpio_pinconfig_get(
        bisen::kVcapAdcGpio, &initial_pin_config);
    BISEN_LOG("BISen ADC diagnostic pad before: gpio=%lu get_status=%lu bsp_cfg=0x%08lx actual_cfg=0x%08lx\n",
              static_cast<unsigned long>(bisen::kVcapAdcGpio),
              static_cast<unsigned long>(initial_pin_status),
              static_cast<unsigned long>(g_AM_BSP_GPIO_ADCSE4.GP.cfg),
              static_cast<unsigned long>(initial_pin_config.GP.cfg));

    uint32_t valid_sample_count = 0u;
    uint32_t corrected_code_sum = 0u;
    uint32_t corrected_code_min = UINT32_MAX;
    uint32_t corrected_code_max = 0u;
    for (uint32_t index = 0u; index < kDiagnosticSamples; ++index) {
        bisen::instrumentation_set_state(bisen::State::kAdc);
        bisen::EnergyObservation observation = {};
        observation.previous_band = bisen::EnergyBand::kWait;
        const bisen::EnergyStatus status = bisen::measure_energy(&observation);

        am_hal_gpio_pincfg_t pin_config = {};
        const uint32_t pin_status = am_hal_gpio_pinconfig_get(
            bisen::kVcapAdcGpio, &pin_config);
        BISEN_LOG("BISen ADC diagnostic SE4 #%lu: status=%u valid=%u prime=%lu code=%lu ADC=%.4f V VCAP_nom=%.4f V VCAP_policy=%.3f V pin_status=%lu pin_cfg=0x%08lx\n",
                  static_cast<unsigned long>(index + 1u),
                  static_cast<unsigned>(status),
                  observation.valid ? 1u : 0u,
                  static_cast<unsigned long>(observation.priming_adc_code),
                  static_cast<unsigned long>(observation.corrected_adc_code),
                  static_cast<double>(observation.adc_volts),
                  static_cast<double>(observation.nominal_vcap_volts),
                  static_cast<double>(observation.vcap_volts),
                  static_cast<unsigned long>(pin_status),
                  static_cast<unsigned long>(pin_config.GP.cfg));
        if (status == bisen::EnergyStatus::kReady && observation.valid) {
            ++valid_sample_count;
            corrected_code_sum += observation.corrected_adc_code;
            if (observation.corrected_adc_code < corrected_code_min) {
                corrected_code_min = observation.corrected_adc_code;
            }
            if (observation.corrected_adc_code > corrected_code_max) {
                corrected_code_max = observation.corrected_adc_code;
            }
        }
        if (index == 0u || index + 1u == kDiagnosticSamples) {
            log_adc_debug_snapshot("SE4", index + 1u);
        }
        am_hal_delay_us(50000u);
    }

    const uint32_t corrected_code_mean_x1000 =
        valid_sample_count == 0u
            ? 0u
            : (corrected_code_sum * 1000u + valid_sample_count / 2u) /
                  valid_sample_count;
    const uint32_t corrected_code_spread =
        valid_sample_count == 0u
            ? 0u
            : corrected_code_max - corrected_code_min;
    BISEN_LOG("BISen ADC repeatability summary: valid=%lu/%lu code_mean_x1000=%lu min=%lu max=%lu spread=%lu\n",
              static_cast<unsigned long>(valid_sample_count),
              static_cast<unsigned long>(kDiagnosticSamples),
              static_cast<unsigned long>(corrected_code_mean_x1000),
              static_cast<unsigned long>(
                  valid_sample_count == 0u ? 0u : corrected_code_min),
              static_cast<unsigned long>(corrected_code_max),
              static_cast<unsigned long>(corrected_code_spread));

    bisen::instrumentation_set_state(bisen::State::kTemperatureSense);
    bisen::TemperatureObservation temperature = {};
    const bisen::TemperatureStatus temperature_status =
        bisen::measure_die_temperature(&temperature);
    BISEN_LOG("BISen ADC diagnostic TEMP: status=%u valid=%u code=%lu sensor=%.4f V temp=%.2f C\n",
              static_cast<unsigned>(temperature_status),
              temperature.valid ? 1u : 0u,
              static_cast<unsigned long>(temperature.corrected_adc_code),
              static_cast<double>(temperature.sensor_volts),
              static_cast<double>(temperature.celsius));
    log_adc_debug_snapshot("TEMP", 1u);
    BISEN_LOG("BISen ADC diagnostic complete; target parked safely\n");
    bisen::instrumentation_set_state(bisen::State::kSleepLowPowerWait);
    while (true) {
        SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
        __DSB();
        __WFI();
    }
}
#endif

void start_next_job(Scheduler *scheduler) {
    const uint32_t completed_job_id = scheduler->context.job_id;
    const uint32_t next_job_id =
        completed_job_id == UINT32_MAX ? 1u : completed_job_id + 1u;
    scheduler->context = bisen::new_context();
    scheduler->context.job_id = next_job_id;
    scheduler->runtime->current_chunk_pixels = 0u;
    // A fresh job with zero completed work has nothing worth recovering.
    // The first successful compute chunk marks the context dirty. This keeps
    // a low-energy transition between jobs from writing a progress=0 record.
    scheduler->decision = {};
    scheduler->restore_attempted = true;
    scheduler->restored_checkpoint_sequence = 0u;
    scheduler->interrupted_checkpoint_detected = false;
    scheduler->hard_failure = false;
    scheduler->bounded_park_allows_continuous = false;
    scheduler->post_compute_measurement = false;
    scheduler->last_progress_rtc_wake = bisen::rtc_wake_count();
    BISEN_LOG("BISen next deterministic job: prior=%lu next=%lu run_mode=%s\n",
              static_cast<unsigned long>(completed_job_id),
              static_cast<unsigned long>(next_job_id),
              scheduler->run_mode == RunMode::kContinuous
                  ? "continuous" : "bounded");
}

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
bool accept_continuous_mode_request(Scheduler *scheduler) {
    if (!g_continuous_mode_button_armed ||
        scheduler->run_mode == RunMode::kContinuous ||
        g_continuous_mode_requested == 0) {
        return false;
    }
    g_continuous_mode_requested = 0;

    // The IRQ flag is only a candidate edge. Require GPIO17 to remain
    // physically low across a debounce interval before changing run mode, so
    // reset/debug/power transients cannot masquerade as a button press.
    uint32_t first_level = 1u;
    uint32_t second_level = 1u;
    const bool pressed =
        am_hal_gpio_state_read(AM_BSP_GPIO_BUTTON0,
                               AM_HAL_GPIO_INPUT_READ,
                               &first_level) == AM_HAL_STATUS_SUCCESS &&
        first_level == 0u;
    ns_delay_us(kModeButtonDebounceUs);
    const bool still_pressed =
        am_hal_gpio_state_read(AM_BSP_GPIO_BUTTON0,
                               AM_HAL_GPIO_INPUT_READ,
                               &second_level) == AM_HAL_STATUS_SUCCESS &&
        second_level == 0u;
    if (!pressed || !still_pressed) {
        BISEN_LOG("BISen BTN0 candidate rejected: GPIO17 was not stably low\n");
        return false;
    }

    g_continuous_mode_button_armed = false;
    uint32_t button_gpio = AM_BSP_GPIO_BUTTON0;
    (void)am_hal_gpio_interrupt_control(
        AM_HAL_GPIO_INT_CHANNEL_0, AM_HAL_GPIO_INT_CTRL_INDV_DISABLE,
        &button_gpio);
    scheduler->run_mode = RunMode::kContinuous;
    append_event(scheduler, bisen::BisenLogEvent::kRunMode,
                 static_cast<uint16_t>(RunMode::kContinuous));
    BISEN_LOG("BISen BTN0 accepted: continuous mode enabled until reset\n");
    return true;
}
#endif

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
bool arm_continuous_mode_button() {
    if (g_continuous_mode_button_armed) {
        return true;
    }

    // A mode change is valid only after bounded operation has parked (complete
    // or safely paused by policy) and the physical switch is observed
    // released. This rejects reset/power-sequence transients and requires a
    // fresh high-to-low edge while parked.
    uint32_t first_level = 0u;
    uint32_t second_level = 0u;
    if (am_hal_gpio_state_read(AM_BSP_GPIO_BUTTON0,
                               AM_HAL_GPIO_INPUT_READ,
                               &first_level) != AM_HAL_STATUS_SUCCESS ||
        first_level == 0u) {
        g_continuous_mode_requested = 0;
        return false;
    }
    ns_delay_us(kModeButtonDebounceUs);
    if (am_hal_gpio_state_read(AM_BSP_GPIO_BUTTON0,
                               AM_HAL_GPIO_INPUT_READ,
                               &second_level) != AM_HAL_STATUS_SUCCESS ||
        second_level == 0u) {
        g_continuous_mode_requested = 0;
        return false;
    }

    uint32_t button_gpio = AM_BSP_GPIO_BUTTON0;
    g_continuous_mode_requested = 0;
    if (am_hal_gpio_interrupt_control(
            AM_HAL_GPIO_INT_CHANNEL_0, AM_HAL_GPIO_INT_CTRL_INDV_ENABLE,
            &button_gpio) != AM_HAL_STATUS_SUCCESS) {
        return false;
    }
    g_continuous_mode_button_armed = true;
    BISEN_LOG("BISen BTN0 armed: bounded job is parked; press SW1 now for continuous mode\n");
    return true;
}
#endif

void restore_after_energy_qualification(Scheduler *scheduler) {
    if (scheduler->restore_attempted) {
        return;
    }
    scheduler->restore_attempted = true;
    bisen::CheckpointInfo info = {};
    bisen::instrumentation_set_code(
        bisen::InstrumentationCode::kContextRestore);
    const bisen::CheckpointStatus status =
        bisen::restore_checkpoint(&scheduler->context, &info);
    bisen::instrumentation_set_state(scheduler->state);
    if (status == bisen::CheckpointStatus::kRestored) {
        bisen::runtime_context_accept_restore(info.sequence);
        scheduler->restored_checkpoint_sequence = info.sequence;
        BISEN_LOG("BISen checkpoint restored: slot=%lu sequence=%lu job=%lu progress=%lu digest=%08lx\n",
                  static_cast<unsigned long>(info.slot_index),
                  static_cast<unsigned long>(info.sequence),
                  static_cast<unsigned long>(scheduler->context.job_id),
                  static_cast<unsigned long>(scheduler->context.compute_index),
                  static_cast<unsigned long>(scheduler->context.workload_digest));
        if (info.interrupted_slot_index != UINT32_MAX) {
            scheduler->interrupted_checkpoint_detected = true;
            BISEN_LOG("BISen interrupted checkpoint ignored: slot=%lu sequence=%lu commit=incomplete\n",
                      static_cast<unsigned long>(info.interrupted_slot_index),
                      static_cast<unsigned long>(info.interrupted_sequence));
        }
#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
        // A completed persistent job is immutable. Start the next numbered
        // job in RAM; bounded mode will stop after that job, while continuous
        // mode will keep advancing through subsequent jobs without reset.
        if (scheduler->context.job_complete) {
            BISEN_LOG("BISen prior completed checkpoint: sequence=%lu\n",
                      static_cast<unsigned long>(info.sequence));
            start_next_job(scheduler);
        }
#endif
    } else {
        scheduler->context = bisen::new_context();
        BISEN_LOG("BISen no valid checkpoint (status=%u); starting deterministic job\n",
                  static_cast<unsigned>(status));
    }
}

bool run_scheduler_step(Scheduler *scheduler) {
    switch (scheduler->state) {
        case bisen::State::kWakeRestore:
            // Never read persistent state until the energy source has passed
            // the explicit bench resume policy.
            set_state(scheduler, bisen::State::kAdc);
            return true;

        case bisen::State::kAdc: {
            const bool post_compute_measurement =
                scheduler->post_compute_measurement;
            scheduler->post_compute_measurement = false;
            bisen::EnergyObservation observation = {};
            observation.previous_band = scheduler->context.energy_band;
            const bisen::EnergyStatus measurement_status =
                bisen::measure_energy(&observation);
            if (measurement_status != bisen::EnergyStatus::kReady) {
                scheduler->hard_failure = true;
                bisen::instrumentation_set_code(
                    bisen::InstrumentationCode::kBootOrError);
                BISEN_LOG("BISen VCAP measurement failed: status=%u prime=%lu code=%lu ADC=%.4f V VCAP_nom=%.4f V VCAP_policy=%.3f V\n",
                          static_cast<unsigned>(measurement_status),
                          static_cast<unsigned long>(observation.priming_adc_code),
                          static_cast<unsigned long>(observation.corrected_adc_code),
                          static_cast<double>(observation.adc_volts),
                          static_cast<double>(observation.nominal_vcap_volts),
                          static_cast<double>(observation.vcap_volts));
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            const bisen::EnergyStatus policy_status =
                bisen::choose_energy_policy(observation, &scheduler->decision);
            if (policy_status != bisen::EnergyStatus::kReady) {
                scheduler->hard_failure = true;
                bisen::instrumentation_set_code(
                    bisen::InstrumentationCode::kBootOrError);
                BISEN_LOG("BISen VCAP policy failed: status=%u\n",
                          static_cast<unsigned>(policy_status));
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            BISEN_LOG("BISen VCAP%s: prime=%lu code=%lu ADC=%.4f V VCAP_nom=%.4f V VCAP_policy=%.3f V plan=%s chunk=%lu restore=%u temp=%u compute=%u below_sleep_floor=%u\n",
                      observation.simulated ? " (SIMULATED)" : "",
                      static_cast<unsigned long>(observation.priming_adc_code),
                      static_cast<unsigned long>(observation.corrected_adc_code),
                      static_cast<double>(observation.adc_volts),
                      static_cast<double>(observation.nominal_vcap_volts),
                      static_cast<double>(observation.vcap_volts),
                      energy_name(scheduler->decision.band),
                      static_cast<unsigned long>(scheduler->decision.chunk_pixels),
                      scheduler->decision.restore_allowed ? 1u : 0u,
                      scheduler->decision.temperature_allowed ? 1u : 0u,
                      scheduler->decision.compute_allowed ? 1u : 0u,
                      scheduler->decision.below_sleep_floor ? 1u : 0u);
            if (!scheduler->restore_attempted &&
                !scheduler->decision.restore_allowed) {
                BISEN_LOG("BISen restore deferred: VCAP below %lu.%03lu V bench resume threshold\n",
                          static_cast<unsigned long>(
                              bisen::kBenchEnergyPolicy.resume_vcap_millivolts /
                              1000u),
                          static_cast<unsigned long>(
                              bisen::kBenchEnergyPolicy.resume_vcap_millivolts %
                              1000u));
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (!scheduler->restore_attempted) {
                restore_after_energy_qualification(scheduler);
            }
            scheduler->context.vcap_millivolts =
                observation.policy_vcap_millivolts;
            scheduler->context.energy_band = scheduler->decision.band;
            scheduler->runtime->current_chunk_pixels =
                scheduler->decision.chunk_pixels;
            append_event(scheduler, bisen::BisenLogEvent::kAdc,
                         static_cast<uint16_t>(scheduler->decision.band));

            const bool temperature_pending =
                (scheduler->context.completion_mask &
                 bisen::kTemperatureComplete) == 0u;
            const bool next_useful_state_allowed =
                (temperature_pending &&
                 scheduler->decision.temperature_allowed) ||
                (!temperature_pending &&
                 !scheduler->context.job_complete &&
                 scheduler->decision.compute_allowed);
            const bisen::PreSleepCheckpointAction checkpoint_action =
                bisen::choose_pre_sleep_checkpoint_action(
                    next_useful_state_allowed,
                    bisen::runtime_context_is_dirty(),
                    scheduler->context.job_complete,
                    bisen::runtime_context_checkpoint_failed());
            if (BISEN_ENABLE_MRAM_CHECKPOINTS &&
                checkpoint_action ==
                    bisen::PreSleepCheckpointAction::kWrite) {
                scheduler->context.resume_state =
                    bisen::State::kSleepLowPowerWait;
                BISEN_LOG("BISen pre-sleep recovery checkpoint requested: next useful state blocked at VCAP_policy=%.3f V progress=%lu generation=%lu\n",
                          static_cast<double>(observation.vcap_volts),
                          static_cast<unsigned long>(
                              scheduler->context.compute_index),
                          static_cast<unsigned long>(
                              scheduler->runtime->runtime_generation));
                set_state(scheduler, bisen::State::kNonvolatileCheckpoint);
                return true;
            }
            if (post_compute_measurement &&
                scheduler->run_mode == RunMode::kBounded) {
                // Keep the laboratory-safe bounded default at one chunk per
                // RTC wake. BTN0 continuous mode instead falls through after
                // this MSP430-style post-chunk ADC sample and immediately runs
                // more useful work only while the policy still permits it.
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (scheduler->decision.below_sleep_floor) {
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (temperature_pending &&
                scheduler->decision.temperature_allowed) {
                set_state(scheduler, bisen::State::kTemperatureSense);
            } else if (!scheduler->context.job_complete &&
                       scheduler->decision.compute_allowed) {
                set_state(scheduler, bisen::State::kCompute);
            } else if (BISEN_ENABLE_MRAM_CHECKPOINTS &&
                       (BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST ||
                        (BISEN_ENABLE_RESET_INJECTION &&
                         scheduler->restored_checkpoint_sequence == 1u &&
                         !scheduler->interrupted_checkpoint_detected))) {
                scheduler->context.resume_state = bisen::State::kSleepLowPowerWait;
                if (BISEN_ENABLE_RESET_INJECTION) {
                    BISEN_LOG("BISen checkpoint reset test: one phase-%u reset armed\n",
                              static_cast<unsigned>(BISEN_RESET_INJECTION_PHASE));
                } else {
                    BISEN_LOG("BISen checkpoint alternation test: one stable-power rewrite requested\n");
                }
                set_state(scheduler, bisen::State::kNonvolatileCheckpoint);
            } else {
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
            }
            return true;
        }

        case bisen::State::kTemperatureSense: {
            bisen::TemperatureObservation temperature = {};
            const bisen::TemperatureStatus temperature_status =
                bisen::measure_die_temperature(&temperature);
            if (temperature_status != bisen::TemperatureStatus::kReady) {
                scheduler->hard_failure = true;
                bisen::instrumentation_set_code(
                    bisen::InstrumentationCode::kBootOrError);
                BISEN_LOG("BISen die-temperature read failed: status=%u; no completion recorded\n",
                          static_cast<unsigned>(temperature_status));
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            scheduler->context.temperature_millicelsius =
                static_cast<int32_t>(temperature.celsius * 1000.0f);
            scheduler->context.completion_mask |= bisen::kTemperatureComplete;
            append_event(scheduler, bisen::BisenLogEvent::kTemperature);
            if (!scheduler->decision.compute_allowed) {
                scheduler->context.resume_state = bisen::State::kSleepLowPowerWait;
            }
            BISEN_LOG("BISen die temperature: code=%lu sensor=%.4f V temp=%.2f C\n",
                      static_cast<unsigned long>(temperature.corrected_adc_code),
                      static_cast<double>(temperature.sensor_volts),
                      static_cast<double>(temperature.celsius));
            // Recheck VCAP after sensing before allowing compute. Temperature
            // observations are reacquired and never make the MRAM context
            // dirty on their own.
            set_state(scheduler, bisen::State::kAdc);
            return true;
        }

        case bisen::State::kCompute: {
            const bisen::ComputeStatus status = bisen::run_compute_chunk(
                &scheduler->context, scheduler->decision.chunk_pixels);
            if (status == bisen::ComputeStatus::kInvalidContext) {
                scheduler->hard_failure = true;
                bisen::instrumentation_set_code(
                    bisen::InstrumentationCode::kBootOrError);
                BISEN_LOG("BISen Sobel context invalid; refusing computation\n");
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            BISEN_LOG("BISen Sobel chunk: progress=%lu/%lu digest=%08lx\n",
                      static_cast<unsigned long>(scheduler->context.compute_index),
                      static_cast<unsigned long>(bisen::sobel_total_pixels()),
                      static_cast<unsigned long>(scheduler->context.workload_digest));
            bisen::runtime_context_note_recovery_change();
            append_event(scheduler, bisen::BisenLogEvent::kCompute);
            scheduler->last_progress_rtc_wake = bisen::rtc_wake_count();
            if (status == bisen::ComputeStatus::kGoldenMismatch) {
                scheduler->hard_failure = true;
                bisen::instrumentation_set_code(
                    bisen::InstrumentationCode::kBootOrError);
                BISEN_LOG("BISen Sobel golden mismatch (expected=%08lx); refusing completion\n",
                          static_cast<unsigned long>(bisen::sobel_golden_digest()));
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (status == bisen::ComputeStatus::kComplete) {
                scheduler->context.completion_mask |= bisen::kComputeComplete;
                scheduler->context.job_complete = true;
                scheduler->context.resume_state = bisen::State::kSleepLowPowerWait;
                BISEN_LOG("BISen Sobel complete: digest=%08lx matches golden\n",
                          static_cast<unsigned long>(
                              scheduler->context.workload_digest));
                if (!BISEN_ENABLE_COMPUTE_RESUME_TEST) {
                    bisen::runtime_context_note_job_complete_without_checkpoint();
                }
            } else {
                scheduler->context.resume_state = bisen::State::kCompute;
            }
            if (BISEN_ENABLE_COMPUTE_RESUME_TEST) {
                // Explicit regression-only mode: preserve the existing
                // manually stepped checkpoint/resume test. It is excluded
                // from integrated production scheduling by compile-time gates.
                set_state(scheduler, bisen::State::kNonvolatileCheckpoint);
            } else {
                scheduler->post_compute_measurement = true;
                set_state(scheduler, bisen::State::kAdc);
            }
            return true;
        }

        case bisen::State::kNonvolatileCheckpoint: {
            bool checkpoint_saved = !BISEN_ENABLE_MRAM_CHECKPOINTS;
            if (BISEN_ENABLE_MRAM_CHECKPOINTS) {
                const bisen::CheckpointLayout layout = bisen::checkpoint_layout();
                bisen::CheckpointInfo info = {};
                bisen::CheckpointStatus status =
                    bisen::CheckpointStatus::kHalError;
                if (bisen::runtime_context_begin_checkpoint_attempt()) {
                    append_event(scheduler,
                                 bisen::BisenLogEvent::kCheckpointAttempt);
                    status = bisen::save_checkpoint(scheduler->context, &info);
                } else {
                    BISEN_LOG("BISen checkpoint attempt blocked: attempts=%lu limit=%u failure_latched=%u\n",
                              static_cast<unsigned long>(
                                  scheduler->runtime->checkpoint_attempt_count),
                              static_cast<unsigned>(
                                  BISEN_MRAM_SESSION_ATTEMPT_LIMIT),
                              bisen::runtime_context_checkpoint_failed()
                                  ? 1u : 0u);
                }
                if (status == bisen::CheckpointStatus::kSaved) {
                    bisen::runtime_context_note_checkpoint_success();
                    append_event(scheduler,
                                 bisen::BisenLogEvent::kCheckpointCommitted);
                    bisen::instrumentation_set_code(
                        bisen::InstrumentationCode::kCheckpointCommitted);
                } else {
                    bisen::runtime_context_note_checkpoint_failure();
                    append_event(scheduler, bisen::BisenLogEvent::kError,
                                 static_cast<uint16_t>(status));
                    bisen::instrumentation_set_code(
                        bisen::InstrumentationCode::kBootOrError);
                }
                BISEN_LOG("BISen checkpoint status=%u slot=%lu sequence=%lu region=[0x%08lx,0x%08lx) slot_bytes=%lu layout=%u\n",
                          static_cast<unsigned>(status),
                          static_cast<unsigned long>(info.slot_index),
                          static_cast<unsigned long>(info.sequence),
                          static_cast<unsigned long>(layout.begin),
                          static_cast<unsigned long>(layout.end),
                          static_cast<unsigned long>(layout.slot_bytes),
                          static_cast<unsigned>(bisen::checkpoint_layout_is_valid()));
                checkpoint_saved = status == bisen::CheckpointStatus::kSaved;
                (void)layout;
            }
            if (!checkpoint_saved) {
                scheduler->hard_failure = true;
                BISEN_LOG("BISen checkpoint save failed; refusing uncheckpointed computation\n");
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (BISEN_ENABLE_COMPUTE_RESUME_TEST &&
                scheduler->context.compute_index > 0u &&
                !scheduler->context.job_complete) {
                BISEN_LOG("BISen compute-resume test: one persisted chunk completed; reset manually to resume\n");
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (BISEN_ENABLE_INTEGRATED_CYCLE_TEST &&
                scheduler->context.compute_index > 0u) {
                BISEN_LOG("BISen low-energy recovery point persisted: progress=%lu/%lu; waiting for VCAP recovery or power loss\n",
                          static_cast<unsigned long>(
                              scheduler->context.compute_index),
                          static_cast<unsigned long>(
                              bisen::sobel_total_pixels()));
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
                return true;
            }
            if (!scheduler->context.job_complete &&
                scheduler->decision.compute_allowed) {
                set_state(scheduler, bisen::State::kCompute);
            } else {
                set_state(scheduler, bisen::State::kSleepLowPowerWait);
            }
            return true;
        }

        case bisen::State::kSleepLowPowerWait: {
            if (BISEN_ENABLE_INTEGRATED_CYCLE_TEST &&
                scheduler->context.job_complete) {
                BISEN_LOG("BISen integrated cycle complete: wakes=%lu progress=%lu digest=%08lx run_mode=%s\n",
                          static_cast<unsigned long>(bisen::rtc_wake_count()),
                          static_cast<unsigned long>(
                              scheduler->context.compute_index),
                          static_cast<unsigned long>(
                              scheduler->context.workload_digest),
                          scheduler->run_mode == RunMode::kContinuous
                              ? "continuous" : "bounded");
                if (scheduler->run_mode == RunMode::kContinuous) {
                    start_next_job(scheduler);
                    set_state(scheduler, bisen::State::kAdc);
                    return true;
                }
                scheduler->bounded_park_allows_continuous = true;
                BISEN_LOG("BISen bounded mode complete: waiting for BTN0/SW1\n");
                return false;
            }
            if (BISEN_ENABLE_INTEGRATED_CYCLE_TEST &&
                scheduler->run_mode == RunMode::kBounded &&
                !scheduler->hard_failure) {
                const uint32_t wakes_since_progress =
                    bisen::rtc_wake_count() -
                    scheduler->last_progress_rtc_wake;
                if (wakes_since_progress >= static_cast<uint32_t>(
                        BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT)) {
                    scheduler->bounded_park_allows_continuous = true;
                    BISEN_LOG("BISen bounded mode paused: no compute progress for %lu RTC wakes (wakes=%lu progress=%lu plan=%s); waiting for BTN0/SW1\n",
                              static_cast<unsigned long>(wakes_since_progress),
                              static_cast<unsigned long>(
                                  bisen::rtc_wake_count()),
                              static_cast<unsigned long>(
                                  scheduler->context.compute_index),
                              energy_name(scheduler->decision.band));
                    return false;
                }
            }
            if (BISEN_ENABLE_INTEGRATED_CYCLE_TEST &&
                scheduler->run_mode == RunMode::kBounded &&
                bisen::rtc_wake_count() >= static_cast<uint32_t>(
                    BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT)) {
                BISEN_LOG("BISen integrated cycle wake limit reached before completion: wakes=%lu progress=%lu; target parked safely\n",
                          static_cast<unsigned long>(bisen::rtc_wake_count()),
                          static_cast<unsigned long>(
                              scheduler->context.compute_index));
                return false;
            }
            if (BISEN_ENABLE_LOW_POWER_TEST &&
                bisen::rtc_wake_count() >=
                    static_cast<uint32_t>(BISEN_LOW_POWER_TEST_WAKE_LIMIT)) {
                BISEN_LOG("BISen bounded low-power test complete: wakes=%lu; target parked safely\n",
                          static_cast<unsigned long>(bisen::rtc_wake_count()));
                return false;
            }
            append_event(scheduler, bisen::BisenLogEvent::kSleep);
            const bisen::SleepStatus sleep = bisen::sleep_until_rtc_alarm();
            if (sleep == bisen::SleepStatus::kWoke) {
                append_event(scheduler, bisen::BisenLogEvent::kWake);
                // The first low-power print reattaches SWO. Give that transport
                // a sacrificial character so the diagnostic line starts whole.
                BISEN_LOG(" \n");
                BISEN_LOG("BISen RTC wake #%lu\n",
                          static_cast<unsigned long>(bisen::rtc_wake_count()));
                set_state(scheduler, bisen::State::kAdc);
                return true;
            }
            if (sleep == bisen::SleepStatus::kDisabled) {
                BISEN_LOG("BISen low-power path compiled out; target parked safely\n");
            } else if (sleep == bisen::SleepStatus::kUnexpectedWake) {
                BISEN_LOG("BISen deep sleep exited without an RTC alarm; target parked safely\n");
            } else {
                scheduler->hard_failure = true;
                bisen::instrumentation_set_code(
                    bisen::InstrumentationCode::kBootOrError);
                BISEN_LOG("BISen RTC/deep-sleep failed: stage=%s hal_status=%lu; target parked safely\n",
                          bisen::rtc_failure_stage_name(
                              bisen::rtc_failure_stage()),
                          static_cast<unsigned long>(
                              bisen::rtc_last_hal_status()));
            }
            return false;
        }
    }
    return false;
}

#if BISEN_ENABLE_RETENTION_TEST
#if BISEN_RETENTION_TRACE_WORKLOAD_REPEATS > 0
volatile uint32_t g_retention_trace_checksum = 0u;

bool run_retention_trace_compute_burst() {
    uint32_t checksum = g_retention_trace_checksum;
    for (uint32_t repetition = 0u;
         repetition <
             static_cast<uint32_t>(BISEN_RETENTION_TRACE_WORKLOAD_REPEATS);
         ++repetition) {
        bisen::Context trace_context = bisen::new_context();
        const bisen::ComputeStatus status = bisen::run_compute_chunk(
            &trace_context, bisen::sobel_total_pixels());
        if (status != bisen::ComputeStatus::kComplete ||
            trace_context.workload_digest != bisen::sobel_golden_digest()) {
            return false;
        }
        // Preserve a visible result dependency so the compiler cannot remove
        // the test-only repeated workloads. This checksum is diagnostics only
        // and is deliberately excluded from retained recovery state.
        checksum = (checksum * 16777619u) ^
                   trace_context.workload_digest ^ repetition;
    }
    g_retention_trace_checksum = checksum;
    return true;
}
#endif

[[noreturn]] void run_retention_test() {
    bisen::BisenRuntimeContext *runtime = bisen::runtime_context();
    runtime->current_chunk_pixels = 100u;
    runtime->context.energy_band = bisen::EnergyBand::kChunk100;

    am_hal_pwrctrl_mcu_memory_config_t memory_config = {};
    const uint32_t memory_status =
        am_hal_pwrctrl_mcu_memory_config_get(&memory_config);
    BISEN_LOG("BISen retention test: wakes=%u chunk=100 MRAM=FORCED-OFF RAM_log=%lu bytes\n",
              static_cast<unsigned>(BISEN_RETENTION_TEST_WAKE_LIMIT),
              static_cast<unsigned long>(bisen::bisen_log_storage_bytes()));
#if BISEN_RETENTION_TRACE_WORKLOAD_REPEATS > 0
    BISEN_LOG("BISen retention scope trace: extra_full_sobel_per_wake=%u state3=real_compute timing=OBSERVABILITY_ONLY\n",
              static_cast<unsigned>(
                  BISEN_RETENTION_TRACE_WORKLOAD_REPEATS));
#endif
    BISEN_LOG("BISen retained region: [0x%08lx,0x%08lx) runtime=0x%08lx log_entries=%lu\n",
              static_cast<unsigned long>(
                  reinterpret_cast<uintptr_t>(__bisen_retained_start__)),
              static_cast<unsigned long>(
                  reinterpret_cast<uintptr_t>(__bisen_retained_end__)),
              static_cast<unsigned long>(reinterpret_cast<uintptr_t>(runtime)),
              static_cast<unsigned long>(bisen::bisen_log_capacity()));
    BISEN_LOG("BISen MCU memory config: status=%lu DTCM=%u retain_DTCM=%u cache=%u retain_cache=%u NVM0=%u retain_NVM0=%u\n",
              static_cast<unsigned long>(memory_status),
              static_cast<unsigned>(memory_config.eDTCMCfg),
              static_cast<unsigned>(memory_config.eRetainDTCM),
              static_cast<unsigned>(memory_config.eCacheCfg),
              memory_config.bRetainCache ? 1u : 0u,
              memory_config.bEnableNVM0 ? 1u : 0u,
              memory_config.bRetainNVM0 ? 1u : 0u);
    if (memory_status != AM_HAL_STATUS_SUCCESS ||
        memory_config.eRetainDTCM == AM_HAL_PWRCTRL_DTCM_NONE) {
        bisen::instrumentation_set_code(
            bisen::InstrumentationCode::kBootOrError);
        BISEN_LOG("BISen retention test FAIL: HAL does not report retained DTCM\n");
        while (true) {
            __WFI();
        }
    }

    for (uint32_t cycle = 0u;
         cycle < static_cast<uint32_t>(BISEN_RETENTION_TEST_WAKE_LIMIT);
         ++cycle) {
        bisen::instrumentation_set_state(bisen::State::kCompute);
        const bisen::ComputeStatus compute =
            bisen::run_compute_chunk(&runtime->context, 100u);
        if (compute != bisen::ComputeStatus::kProgress) {
            bisen::instrumentation_set_code(
                bisen::InstrumentationCode::kBootOrError);
            BISEN_LOG("BISen retention test FAIL: compute status=%u cycle=%lu\n",
                      static_cast<unsigned>(compute),
                      static_cast<unsigned long>(cycle));
            while (true) {
                __WFI();
            }
        }
#if BISEN_RETENTION_TRACE_WORKLOAD_REPEATS > 0
        if (!run_retention_trace_compute_burst()) {
            bisen::instrumentation_set_code(
                bisen::InstrumentationCode::kBootOrError);
            BISEN_LOG("BISen retention scope trace FAIL: repeated Sobel golden mismatch cycle=%lu\n",
                      static_cast<unsigned long>(cycle));
            while (true) {
                __WFI();
            }
        }
#endif
        runtime->context.resume_state = bisen::State::kCompute;
        bisen::runtime_context_note_recovery_change();
        bisen::bisen_log_append(bisen::BisenLogEvent::kCompute, 0u,
                                bisen::rtc_wake_count(), *runtime);

        const uint32_t expected_progress = runtime->context.compute_index;
        const uint32_t expected_digest = runtime->context.workload_digest;
        const uint32_t expected_generation = runtime->runtime_generation;
        const bisen::EnergyBand expected_band = runtime->context.energy_band;
        const uint32_t expected_chunk = runtime->current_chunk_pixels;
        bisen::instrumentation_set_state(bisen::State::kSleepLowPowerWait);
        bisen::bisen_log_append(bisen::BisenLogEvent::kSleep, 0u,
                                bisen::rtc_wake_count(), *runtime);
        const uint32_t expected_log_count = bisen::bisen_log_count();
        bisen::BisenLogEntry expected_tail = {};
        const bool tail_available = bisen::bisen_log_read_oldest(
            expected_log_count - 1u, &expected_tail);

        const bisen::SleepStatus sleep = bisen::sleep_until_rtc_alarm();
        bisen::BisenLogEntry retained_tail = {};
        const bool retained_tail_available = bisen::bisen_log_read_oldest(
            expected_log_count - 1u, &retained_tail);
        const bool retained =
            sleep == bisen::SleepStatus::kWoke && tail_available &&
            retained_tail_available &&
            runtime->context.compute_index == expected_progress &&
            runtime->context.workload_digest == expected_digest &&
            runtime->runtime_generation == expected_generation &&
            runtime->committed_generation == 0u &&
            runtime->context.energy_band == expected_band &&
            runtime->current_chunk_pixels == expected_chunk &&
            bisen::bisen_log_count() == expected_log_count &&
            retained_tail.sequence == expected_tail.sequence &&
            retained_tail.event == expected_tail.event &&
            retained_tail.progress == expected_tail.progress &&
            retained_tail.digest == expected_tail.digest;
        if (!retained) {
            bisen::instrumentation_set_code(
                bisen::InstrumentationCode::kBootOrError);
            BISEN_LOG("BISen retention test FAIL: cycle=%lu sleep=%u progress=%lu/%lu digest=%08lx/%08lx generation=%lu/%lu plan=%u/%u chunk=%lu/%lu log_count=%lu/%lu\n",
                      static_cast<unsigned long>(cycle + 1u),
                      static_cast<unsigned>(sleep),
                      static_cast<unsigned long>(runtime->context.compute_index),
                      static_cast<unsigned long>(expected_progress),
                      static_cast<unsigned long>(runtime->context.workload_digest),
                      static_cast<unsigned long>(expected_digest),
                      static_cast<unsigned long>(runtime->runtime_generation),
                      static_cast<unsigned long>(expected_generation),
                      static_cast<unsigned>(runtime->context.energy_band),
                      static_cast<unsigned>(expected_band),
                      static_cast<unsigned long>(
                          runtime->current_chunk_pixels),
                      static_cast<unsigned long>(expected_chunk),
                      static_cast<unsigned long>(bisen::bisen_log_count()),
                      static_cast<unsigned long>(expected_log_count));
            while (true) {
                __WFI();
            }
        }

        bisen::bisen_log_append(bisen::BisenLogEvent::kWake, 0u,
                                bisen::rtc_wake_count(), *runtime);
        BISEN_LOG(" \n");
#if BISEN_RETENTION_TRACE_WORKLOAD_REPEATS > 0
        BISEN_LOG("BISen retention wake #%lu PASS: progress=%lu digest=%08lx generation=%lu committed=%lu log_count=%lu trace=%08lx\n",
                  static_cast<unsigned long>(bisen::rtc_wake_count()),
                  static_cast<unsigned long>(runtime->context.compute_index),
                  static_cast<unsigned long>(runtime->context.workload_digest),
                  static_cast<unsigned long>(runtime->runtime_generation),
                  static_cast<unsigned long>(runtime->committed_generation),
                  static_cast<unsigned long>(bisen::bisen_log_count()),
                  static_cast<unsigned long>(g_retention_trace_checksum));
#else
        BISEN_LOG("BISen retention wake #%lu PASS: progress=%lu digest=%08lx generation=%lu committed=%lu log_count=%lu\n",
                  static_cast<unsigned long>(bisen::rtc_wake_count()),
                  static_cast<unsigned long>(runtime->context.compute_index),
                  static_cast<unsigned long>(runtime->context.workload_digest),
                  static_cast<unsigned long>(runtime->runtime_generation),
                  static_cast<unsigned long>(runtime->committed_generation),
                  static_cast<unsigned long>(bisen::bisen_log_count()));
#endif
    }

    bisen::instrumentation_set_state(bisen::State::kSleepLowPowerWait);
    BISEN_LOG("BISen retention test PASS: wakes=%lu progress=%lu generation=%lu committed=%lu dirty=%u MRAM_attempts=%lu MRAM_successes=%lu log_count=%lu wraps=%lu; target parked safely\n",
              static_cast<unsigned long>(bisen::rtc_wake_count()),
              static_cast<unsigned long>(runtime->context.compute_index),
              static_cast<unsigned long>(runtime->runtime_generation),
              static_cast<unsigned long>(runtime->committed_generation),
              bisen::runtime_context_is_dirty() ? 1u : 0u,
              static_cast<unsigned long>(runtime->checkpoint_attempt_count),
              static_cast<unsigned long>(runtime->checkpoint_success_count),
              static_cast<unsigned long>(bisen::bisen_log_count()),
              static_cast<unsigned long>(bisen::bisen_log_wrap_count()));
    while (true) {
        __WFI();
    }
}
#endif

}  // namespace

int main(void) {
    ns_core_config_t core_config = {.api = &ns_core_V1_0_0};
    NS_TRY(ns_core_init(&core_config), "BISen core init failed.\n");
    NS_TRY(ns_power_config(&ns_development_default), "BISen power init failed.\n");

#if BISEN_ENABLE_DEBUG_LOGGING
    ns_itm_printf_enable();
#endif

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
    // Runtime mode must be selected by an edge that occurs after button
    // initialization. Do not infer a press from the boot-time pin level: the
    // external-power/debugger startup sequence can momentarily present a low
    // level, and a held button must not make reset enter an unbounded run.
    g_continuous_mode_requested = 0;
    ns_button_config_t button_config = {
        .api = &ns_button_V1_0_0,
        .button_0_enable = true,
        .button_1_enable = false,
        .joulescope_trigger_enable = false,
        .button_0_flag = &g_continuous_mode_requested,
        .button_1_flag = nullptr,
        .joulescope_trigger_flag = nullptr,
    };
    NS_TRY(ns_peripheral_button_init(&button_config),
           "BISen BTN0 initialization failed.\n");
    // Keep the interrupt disabled until initial bounded operation parks.
    // Discard any edge generated while the GPIO is being configured.
    uint32_t button_gpio = AM_BSP_GPIO_BUTTON0;
    NS_TRY(am_hal_gpio_interrupt_control(
               AM_HAL_GPIO_INT_CHANNEL_0,
               AM_HAL_GPIO_INT_CTRL_INDV_DISABLE,
               &button_gpio),
           "BISen BTN0 interrupt-disable failed.\n");
    g_continuous_mode_requested = 0;
    g_continuous_mode_button_armed = false;
#endif

    const bisen::InstrumentationStatus instrumentation_status =
        bisen::instrumentation_init();
    if (instrumentation_status != bisen::InstrumentationStatus::kReady &&
        instrumentation_status != bisen::InstrumentationStatus::kDisabled) {
        BISEN_LOG("BISen GPIO instrumentation initialization failed: status=%u\n",
                  static_cast<unsigned>(instrumentation_status));
        while (true) {
        }
    }

    bisen::runtime_context_reset();
    bisen::bisen_log_reset();
    bisen::BisenRuntimeContext *runtime = bisen::runtime_context();
    bisen::bisen_log_append(bisen::BisenLogEvent::kBoot, 0u, 0u, *runtime);

#if BISEN_ENABLE_RETENTION_TEST
    run_retention_test();
#endif

    BISEN_LOG("BISen Apollo4 port: staged ADC/temp/scheduler/MRAM/RTC/Sobel build\n");
#if BISEN_ENABLE_GPIO_INSTRUMENTATION
    BISEN_LOG("BISen GPIO state bus: STATE0=J9.7/GPIO62 STATE1=J9.9/GPIO63 STATE2=J9.11/GPIO61\n");
#endif
#if BISEN_ENABLE_LOW_POWER_TEST
    BISEN_LOG("BISen bounded low-power test: LFRC RTC interval=1 s wake_limit=%u MRAM=off compute=off\n",
              static_cast<unsigned>(BISEN_LOW_POWER_TEST_WAKE_LIMIT));
#endif
#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
    BISEN_LOG("BISen integrated cycle test: ADC+MRAM+Sobel+LFRC RTC max_wakes=%u\n",
              static_cast<unsigned>(
                  BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT));
    BISEN_LOG("BISen run-mode guard v4: bounded first; no-progress_wakes=%u; BTN0 requires a debounced press after park\n",
              static_cast<unsigned>(
                  BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT));
#endif
#if BISEN_ENABLE_ADC_DIAGNOSTIC
    run_adc_diagnostic();
#endif
    Scheduler scheduler = {
        .state = bisen::State::kWakeRestore,
        .runtime = runtime,
        .context = runtime->context,
        .decision = {},
        .restore_attempted = false,
        .restored_checkpoint_sequence = 0u,
        .interrupted_checkpoint_detected = false,
        .hard_failure = false,
        .bounded_park_allows_continuous = false,
        .post_compute_measurement = false,
        .last_progress_rtc_wake = 0u,
        .run_mode = RunMode::kBounded,
    };
    BISEN_LOG("BISen state: %s\n", state_name(scheduler.state));
    while (true) {
        while (run_scheduler_step(&scheduler)) {
        }
#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST
        if (scheduler.run_mode == RunMode::kBounded) {
            if (scheduler.hard_failure ||
                (!scheduler.context.job_complete &&
                 !scheduler.bounded_park_allows_continuous)) {
                // Peripheral, compute, checkpoint, RTC, and hard wake-limit
                // failures remain fail-closed. A policy-qualified no-progress
                // pause is not a failure and may still arm BTN0.
                while (true) {
                    __WFI();
                }
            }
            while (scheduler.run_mode == RunMode::kBounded) {
                if (!g_continuous_mode_button_armed &&
                    !arm_continuous_mode_button()) {
                    // BTN0 is still held or its level could not be read. The
                    // one-second RTC remains available to wake and retry after
                    // release without a busy loop or an MRAM write.
                    __WFI();
                    continue;
                }
                (void)accept_continuous_mode_request(&scheduler);
                if (scheduler.run_mode == RunMode::kContinuous) {
                    break;
                }
                // BTN0 is interrupt-driven. Close the flag/WFI race so the
                // bounded park consumes no busy-loop CPU while awaiting it.
                const uint32_t interrupt_state =
                    am_hal_interrupt_master_disable();
                if (g_continuous_mode_requested == 0) {
                    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
                    __DSB();
                    __WFI();
                }
                am_hal_interrupt_master_set(interrupt_state);
            }
            if (scheduler.context.job_complete) {
                start_next_job(&scheduler);
            }
            set_state(&scheduler, bisen::State::kAdc);
            continue;
        }
#endif
        // Non-integrated builds and hard failures retain the original safe
        // park behavior. Continuous integrated operation does not return here
        // on normal job completion.
        while (true) {
            __WFI();
        }
    }
}
