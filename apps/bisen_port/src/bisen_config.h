#ifndef BISEN_CONFIG_H
#define BISEN_CONFIG_H

#include <stdint.h>

// Hardware-facing defaults are deliberately conservative. Enabling the VCAP
// path requires confirmation that the Rev. 2.0 divider is connected to
// J9 pin 10 / GPIO15 / ADCSE4. GPIO15 is also connected through fitted SB60 to
// U15 ON2 and has a fitted 1 Mohm pull-down (R51), which is included below.
#ifndef BISEN_ENABLE_DEBUG_LOGGING
#define BISEN_ENABLE_DEBUG_LOGGING 1
#endif

#ifndef BISEN_ENABLE_VCAP_ADC
#define BISEN_ENABLE_VCAP_ADC 0
#endif

#ifndef BISEN_VCAP_ADC_PIN_CONFIRMED
#define BISEN_VCAP_ADC_PIN_CONFIRMED 0
#endif

#ifndef BISEN_ENABLE_ADC_DIAGNOSTIC
#define BISEN_ENABLE_ADC_DIAGNOSTIC 0
#endif

#ifndef BISEN_TEST_VCAP_MV
#define BISEN_TEST_VCAP_MV 0
#endif

#ifndef BISEN_ENABLE_LOW_POWER
#define BISEN_ENABLE_LOW_POWER 0
#endif

#ifndef BISEN_ENABLE_LOW_POWER_TEST
#define BISEN_ENABLE_LOW_POWER_TEST 0
#endif

#ifndef BISEN_LOW_POWER_TEST_WAKE_LIMIT
#define BISEN_LOW_POWER_TEST_WAKE_LIMIT 3
#endif

#ifndef BISEN_ENABLE_INTEGRATED_CYCLE_TEST
#define BISEN_ENABLE_INTEGRATED_CYCLE_TEST 0
#endif

#ifndef BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT
#define BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT 50
#endif

#ifndef BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT
#define BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT 3
#endif

#ifndef BISEN_ENABLE_MRAM_CHECKPOINTS
#define BISEN_ENABLE_MRAM_CHECKPOINTS 0
#endif

#ifndef BISEN_ENABLE_COMPUTE_WORKLOAD
#define BISEN_ENABLE_COMPUTE_WORKLOAD 0
#endif

#ifndef BISEN_ENABLE_COMPUTE_RESUME_TEST
#define BISEN_ENABLE_COMPUTE_RESUME_TEST 0
#endif

#ifndef BISEN_ENABLE_RESET_INJECTION
#define BISEN_ENABLE_RESET_INJECTION 0
#endif

#ifndef BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST
#define BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST 0
#endif

#ifndef BISEN_RESET_INJECTION_PHASE
#define BISEN_RESET_INJECTION_PHASE 0
#endif

#ifndef BISEN_ENABLE_GPIO_INSTRUMENTATION
#define BISEN_ENABLE_GPIO_INSTRUMENTATION 0
#endif

#define BISEN_LOG_BACKEND_NONE 0
#define BISEN_LOG_BACKEND_RAM 1
#define BISEN_LOG_BACKEND_MRAM 2

#ifndef BISEN_LOG_BACKEND
#define BISEN_LOG_BACKEND BISEN_LOG_BACKEND_RAM
#endif

#ifndef BISEN_MRAM_SESSION_ATTEMPT_LIMIT
#define BISEN_MRAM_SESSION_ATTEMPT_LIMIT 64
#endif

#ifndef BISEN_RESUME_VCAP_MV
#define BISEN_RESUME_VCAP_MV 6100
#endif

#ifndef BISEN_FORCE_DISABLE_MRAM_PROGRAMMING
#define BISEN_FORCE_DISABLE_MRAM_PROGRAMMING 0
#endif

#ifndef BISEN_ENABLE_RETENTION_TEST
#define BISEN_ENABLE_RETENTION_TEST 0
#endif

#ifndef BISEN_RETENTION_TEST_WAKE_LIMIT
#define BISEN_RETENTION_TEST_WAKE_LIMIT 3
#endif

// Optional scope-observability load for the retained-RAM test. Each repeat is
// one complete, verified 3,844-pixel Sobel workload executed while state code
// 3 is asserted. Zero preserves the timing of the original retention test.
// Nonzero values are test-only and must never be used for energy claims.
#ifndef BISEN_RETENTION_TRACE_WORKLOAD_REPEATS
#define BISEN_RETENTION_TRACE_WORKLOAD_REPEATS 0
#endif

#if BISEN_LOG_BACKEND < BISEN_LOG_BACKEND_NONE || \
    BISEN_LOG_BACKEND > BISEN_LOG_BACKEND_MRAM
#error "Unknown BISEN_LOG_BACKEND"
#endif

#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_MRAM
#error "Persistent event logging is intentionally unsupported in this refactor"
#endif

#if BISEN_MRAM_SESSION_ATTEMPT_LIMIT < 1 || \
    BISEN_MRAM_SESSION_ATTEMPT_LIMIT > 64
#error "The powered-session MRAM attempt limit must be between 1 and 64"
#endif

#if BISEN_RESUME_VCAP_MV > 9040
#error "VCAP policy values must remain inside the 9.04 V validated ADC range"
#endif

#if BISEN_FORCE_DISABLE_MRAM_PROGRAMMING && BISEN_ENABLE_MRAM_CHECKPOINTS
#error "Forced MRAM-programming disable requires checkpoints to be compiled out"
#endif

#if BISEN_ENABLE_RETENTION_TEST && \
    (!BISEN_ENABLE_DEBUG_LOGGING || !BISEN_ENABLE_LOW_POWER || \
     !BISEN_ENABLE_COMPUTE_WORKLOAD || BISEN_ENABLE_MRAM_CHECKPOINTS || \
     !BISEN_FORCE_DISABLE_MRAM_PROGRAMMING || \
     BISEN_LOG_BACKEND != BISEN_LOG_BACKEND_RAM || \
     BISEN_ENABLE_LOW_POWER_TEST || BISEN_ENABLE_INTEGRATED_CYCLE_TEST || \
     BISEN_ENABLE_COMPUTE_RESUME_TEST || BISEN_ENABLE_RESET_INJECTION || \
     BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST)
#error "Retention test requires debug/low-power/compute/RAM-log on, MRAM forcibly off, and all other test modes off"
#endif

#if BISEN_ENABLE_RETENTION_TEST && BISEN_RETENTION_TEST_WAKE_LIMIT < 3
#error "Retention test requires at least three RTC deep-sleep wakes"
#endif

#if !BISEN_ENABLE_RETENTION_TEST && \
    BISEN_RETENTION_TRACE_WORKLOAD_REPEATS != 0
#error "Retention trace workloads are valid only in the retention test"
#endif

#if BISEN_ENABLE_RETENTION_TEST && \
    BISEN_RETENTION_TRACE_WORKLOAD_REPEATS > 0 && \
    !BISEN_ENABLE_GPIO_INSTRUMENTATION
#error "Retention trace workloads require GPIO state instrumentation"
#endif

#if BISEN_RETENTION_TRACE_WORKLOAD_REPEATS > 1024
#error "Retention trace workload repeats must be between 0 and 1024"
#endif

#if BISEN_ENABLE_VCAP_ADC && !BISEN_VCAP_ADC_PIN_CONFIRMED
#error "Set BISEN_VCAP_ADC_PIN_CONFIRMED=1 only after the divider is connected to Rev. 2.0 J9 pin 10 / GPIO15"
#endif

#if BISEN_ENABLE_VCAP_ADC && BISEN_TEST_VCAP_MV
#error "Use either the physical VCAP ADC or BISEN_TEST_VCAP_MV, never both"
#endif

#if BISEN_ENABLE_ADC_DIAGNOSTIC && \
    (!BISEN_ENABLE_DEBUG_LOGGING || !BISEN_ENABLE_VCAP_ADC || \
     !BISEN_VCAP_ADC_PIN_CONFIRMED || BISEN_TEST_VCAP_MV || \
     BISEN_ENABLE_LOW_POWER || BISEN_ENABLE_LOW_POWER_TEST || \
     BISEN_ENABLE_INTEGRATED_CYCLE_TEST || \
     BISEN_ENABLE_MRAM_CHECKPOINTS || BISEN_ENABLE_COMPUTE_WORKLOAD || \
     !BISEN_FORCE_DISABLE_MRAM_PROGRAMMING || \
     BISEN_ENABLE_COMPUTE_RESUME_TEST || BISEN_ENABLE_RESET_INJECTION || \
     BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST || \
     BISEN_ENABLE_RETENTION_TEST)
#error "ADC diagnostic requires physical ADC/logging only; MRAM must be forcibly disabled and low-power, compute, simulation, and all other tests must be off"
#endif

#if BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST && !BISEN_ENABLE_MRAM_CHECKPOINTS
#error "Checkpoint alternation testing requires BISEN_ENABLE_MRAM_CHECKPOINTS=1"
#endif

#if BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST && \
    (BISEN_ENABLE_COMPUTE_WORKLOAD || BISEN_ENABLE_LOW_POWER || \
     BISEN_ENABLE_RESET_INJECTION || !BISEN_ENABLE_DEBUG_LOGGING)
#error "Checkpoint alternation testing requires compute/low-power/reset-injection off and debug logging on"
#endif

#if BISEN_ENABLE_RESET_INJECTION && !BISEN_ENABLE_MRAM_CHECKPOINTS
#error "Reset injection requires BISEN_ENABLE_MRAM_CHECKPOINTS=1"
#endif

#if BISEN_ENABLE_RESET_INJECTION && \
    (BISEN_RESET_INJECTION_PHASE < 2 || BISEN_RESET_INJECTION_PHASE > 3)
#error "The one-shot checkpoint reset test supports only phase 2 or phase 3"
#endif

#if BISEN_ENABLE_RESET_INJECTION && \
    (BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST || \
     BISEN_ENABLE_COMPUTE_WORKLOAD || BISEN_ENABLE_LOW_POWER || \
     !BISEN_ENABLE_DEBUG_LOGGING)
#error "Reset injection testing requires alternation/compute/low-power off and debug logging on"
#endif

#if BISEN_ENABLE_COMPUTE_RESUME_TEST && \
    (!BISEN_ENABLE_COMPUTE_WORKLOAD || !BISEN_ENABLE_MRAM_CHECKPOINTS)
#error "The compute-resume test requires both compute and MRAM checkpoints"
#endif

#if BISEN_ENABLE_COMPUTE_RESUME_TEST && \
    (BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST || \
     BISEN_ENABLE_RESET_INJECTION || BISEN_ENABLE_LOW_POWER || \
     !BISEN_ENABLE_DEBUG_LOGGING)
#error "The compute-resume test requires alternation/reset/low-power off and debug logging on"
#endif

#if BISEN_ENABLE_LOW_POWER_TEST && !BISEN_ENABLE_LOW_POWER
#error "The bounded low-power test requires BISEN_ENABLE_LOW_POWER=1"
#endif

#if BISEN_ENABLE_LOW_POWER_TEST && BISEN_LOW_POWER_TEST_WAKE_LIMIT < 1
#error "The bounded low-power test requires at least one RTC wake"
#endif

#if BISEN_ENABLE_LOW_POWER_TEST && \
    (!BISEN_ENABLE_VCAP_ADC || !BISEN_VCAP_ADC_PIN_CONFIRMED || \
     BISEN_ENABLE_MRAM_CHECKPOINTS || BISEN_ENABLE_COMPUTE_WORKLOAD || \
     BISEN_ENABLE_COMPUTE_RESUME_TEST || BISEN_ENABLE_RESET_INJECTION || \
     BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST || !BISEN_ENABLE_DEBUG_LOGGING)
#error "The bounded low-power test requires confirmed ADC/logging on and MRAM/compute/reset tests off"
#endif

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST && \
    (!BISEN_ENABLE_VCAP_ADC || !BISEN_VCAP_ADC_PIN_CONFIRMED || \
     !BISEN_ENABLE_LOW_POWER || !BISEN_ENABLE_MRAM_CHECKPOINTS || \
     !BISEN_ENABLE_COMPUTE_WORKLOAD || BISEN_ENABLE_LOW_POWER_TEST || \
     BISEN_ENABLE_COMPUTE_RESUME_TEST || BISEN_ENABLE_RESET_INJECTION || \
     BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST || \
     (!BISEN_ENABLE_DEBUG_LOGGING && !BISEN_ENABLE_GPIO_INSTRUMENTATION))
#error "The integrated-cycle test requires confirmed ADC/low-power/MRAM/compute, one observability backend, and all other tests off"
#endif

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST && \
    BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT < 1
#error "The integrated-cycle test requires at least one allowed RTC wake"
#endif

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST && \
    BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT < 1
#error "The integrated-cycle test requires at least one no-progress RTC wake"
#endif

#if BISEN_ENABLE_INTEGRATED_CYCLE_TEST && \
    BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT > \
        BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT
#error "The bounded no-progress limit cannot exceed the integrated wake limit"
#endif

namespace bisen {

// Resolved from the installed apollo4p_blue_kxr_evb R4.5.0 BSP and the
// AMAP4BPXEVB Rev. 2.0 schematic. Its physical hookup remains compile-gated.
constexpr uint32_t kVcapAdcGpio = 15u;

// Rev. 2.0 J9 state bus selected after schematic, assembly, BSP, and current
// application-use review. The three adjacent odd-row header contacts have no
// fitted LED, button, J-Link UART, SWO, or VCAP role. Their alternate UART3 /
// IOM6 functions are unavailable while instrumentation owns them as GPIOs.
constexpr uint32_t kInstrumentationState0Gpio = 62u;  // J9 pin 7
constexpr uint32_t kInstrumentationState1Gpio = 63u;  // J9 pin 9
constexpr uint32_t kInstrumentationState2Gpio = 61u;  // J9 pin 11
static_assert(kInstrumentationState0Gpio != kVcapAdcGpio,
              "STATE0 must not share the VCAP ADC pin");
static_assert(kInstrumentationState1Gpio != kVcapAdcGpio,
              "STATE1 must not share the VCAP ADC pin");
static_assert(kInstrumentationState2Gpio != kVcapAdcGpio,
              "STATE2 must not share the VCAP ADC pin");
constexpr float kAdcReferenceVolts = 1.19f;
constexpr float kVcapDividerTopKohms = 1000.0f;
constexpr float kVcapDividerBottomKohms = 55.8f;
constexpr float kVcapBoardPulldownKohms = 1000.0f;
// R51 is in parallel with the external 55.8 kohm lower leg. Expressing the
// scale as conductances avoids hiding that board load in a rounded equivalent.
constexpr float kVcapDividerScale =
    1.0f + kVcapDividerTopKohms / kVcapDividerBottomKohms +
    kVcapDividerTopKohms / kVcapBoardPulldownKohms;
// Bench calibration for energy-policy decisions. The fit uses every corrected
// ADC sample collected at the five DMM-confirmed VCAP points (5.00, 5.70,
// 6.10, 7.50, and 9.00 V), including the two 16-sample threshold runs. In
// millivolts:
//
//   VCAP = (5.680275 * corrected_code) + 213.104123
//
// Integer nanovolt coefficients make the scheduler comparison deterministic
// and avoid treating a rounded, nominal divider estimate as a measured bench
// voltage. The nominal 1.19 V/divider conversion remains the independent
// physical-range safety check below.
constexpr uint64_t kVcapCalibrationNanovoltsPerCode = 5680275ull;
constexpr uint64_t kVcapCalibrationOffsetNanovolts = 213104123ull;
constexpr uint64_t kNanovoltsPerMillivolt = 1000000ull;

inline uint32_t calibrated_vcap_millivolts_from_code(uint32_t corrected_code) {
    const uint64_t calibrated_nanovolts =
        kVcapCalibrationNanovoltsPerCode * corrected_code +
        kVcapCalibrationOffsetNanovolts;
    return static_cast<uint32_t>(
        (calibrated_nanovolts + kNanovoltsPerMillivolt / 2u) /
        kNanovoltsPerMillivolt);
}
// GPIO15 is shared with U15 ON2. The TPS22976 specifies 0.5 V as the maximum
// guaranteed-low input voltage. The physical validation range is explicitly
// capped at the bench-confirmed 9.04 V; violating either limit must fail closed.
constexpr float kVcapAdcMaxValidationVolts = 0.5f;
constexpr float kVcapMaxValidationVolts = 9.04f;
constexpr uint32_t kVcapAdcSettleUs = 5000u;
// TRKCYC is a six-bit Apollo4 ADC slot field. Use its maximum value for the
// divider's approximately 50.3 kohm Thevenin source resistance.
constexpr uint32_t kVcapAdcTrackingCycles = 63u;

// These are explicitly bench-policy values from the handoff, not Apollo4
// electrical limits. They are centralized so later capacitor/converter
// characterization can replace them without spreading threshold guesses
// through the scheduler. The names describe exact scheduler actions rather
// than qualitative computation modes: every selected chunk runs the same
// deterministic Sobel kernel.
struct EnergyPolicyConfig {
    uint32_t chunk_1000_min_vcap_millivolts;
    uint32_t chunk_500_min_vcap_millivolts;
    uint32_t chunk_100_min_vcap_millivolts;
    uint32_t sleep_below_vcap_millivolts;
    uint32_t resume_vcap_millivolts;
    uint32_t chunk_1000_pixels;
    uint32_t chunk_500_pixels;
    uint32_t chunk_100_pixels;
};

constexpr EnergyPolicyConfig kBenchEnergyPolicy = {
    8500u, 6400u, 5900u,
    5500u,
    BISEN_RESUME_VCAP_MV,
    1000u, 500u, 100u,
};

enum class State : uint8_t {
    kWakeRestore = 0,
    kAdc,
    kTemperatureSense,
    kCompute,
    kNonvolatileCheckpoint,
    kSleepLowPowerWait,
};

enum class EnergyBand : uint8_t {
    kWait = 0,
    kChunk100,
    kChunk500,
    kChunk1000,
};

enum Completion : uint32_t {
    kTemperatureComplete = 1u << 0,
    kComputeComplete = 1u << 1,
};

constexpr uint32_t kWorkloadInitialDigest = 2166136261u;

struct Context {
    uint32_t job_id;
    uint32_t completion_mask;
    uint32_t compute_index;
    uint32_t output_progress;
    uint32_t workload_digest;
    int32_t temperature_millicelsius;
    uint32_t vcap_millivolts;
    State resume_state;
    EnergyBand energy_band;
    bool job_complete;
};

inline Context new_context() {
    Context context = {};
    context.job_id = 1u;
    context.resume_state = State::kAdc;
    context.energy_band = EnergyBand::kWait;
    context.workload_digest = kWorkloadInitialDigest;
    return context;
}

}  // namespace bisen

#endif  // BISEN_CONFIG_H
