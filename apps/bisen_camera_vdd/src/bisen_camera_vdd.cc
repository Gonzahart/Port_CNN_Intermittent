// BISen camera/CNN direct-VDD integration for AMAP4PEVB Rev. 1 / apollo4p_evb.
//
// The camera package owns sensing and the resumable LeNet engine. This file
// owns the energy decision, state transitions, and when recovery state is
// written. apps/bisen_port is intentionally not linked or modified.
#include <stdint.h>
#include <string.h>
#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "am_util.h"
#include "adc_shared.h"
#include "bisen/bisen_config.h"
#include "ckpt.h"
#include "energy_source.h"
#include "infer.h"
#include "ns_ambiqsuite_harness.h"
#include "ns_core.h"
#include "ns_peripherals_button.h"
#include "ns_peripherals_power.h"
#include "power.h"
#include "power_policy.h"
#include "scan.h"
#include "sensor.h"
#include "wl_apitest.h"
#include "workload.h"

#ifndef BISEN_CAMERA_ENABLE_MRAM
#define BISEN_CAMERA_ENABLE_MRAM 1
#endif
#ifndef BISEN_CAMERA_MAX_CHECKPOINTS
#define BISEN_CAMERA_MAX_CHECKPOINTS 0
#endif
#ifndef BISEN_CAMERA_WAIT_US
#define BISEN_CAMERA_WAIT_US 250000u
#endif
#ifndef BISEN_CAMERA_MAX_WAIT_CYCLES
#define BISEN_CAMERA_MAX_WAIT_CYCLES 40u
#endif
#ifndef BISEN_CAMERA_AUTORUN
#define BISEN_CAMERA_AUTORUN 1
#endif
#ifndef BISEN_CAMERA_SCAN_MAX_UNITS
#define BISEN_CAMERA_SCAN_MAX_UNITS 1u
#endif
#if BISEN_CAMERA_SCAN_MAX_UNITS < 1
#error "BISEN_CAMERA_SCAN_MAX_UNITS must preserve at least one coherent pixel"
#endif
#ifndef WL_APITEST
#define WL_APITEST 0
#endif
#ifndef BISEN_CAMERA_VDD_CALIBRATION_MODE
#define BISEN_CAMERA_VDD_CALIBRATION_MODE 1
#endif
#ifndef BISEN_CAMERA_VDD_CALIBRATION_SAMPLES
#define BISEN_CAMERA_VDD_CALIBRATION_SAMPLES 32u
#endif
#if BISEN_CAMERA_VDD_CALIBRATION_SAMPLES < 2
#error "VDD calibration needs at least two ADC samples per DMM setpoint"
#endif
#if BISEN_CAMERA_VDD_CALIBRATION_MODE && BISEN_CAMERA_ENABLE_MRAM
#error "VDD calibration mode requires BISEN_CAMERA_ENABLE_MRAM=0"
#endif

namespace {

volatile int g_button0_pressed;
volatile int g_button1_pressed;
uint32_t g_checkpoint_attempts;
uint32_t g_checkpoint_successes;
uint32_t g_checkpoint_retirements;
bool g_checkpoint_failure_latched;
bool g_job_tracking_active;
bool g_job_has_durable_checkpoint;
bool g_checkpoint_edge_armed;
uint32_t g_job_generation;
uint32_t g_runtime_generation;
uint32_t g_committed_generation;

#if BISEN_CAMERA_VDD_CALIBRATION_MODE
constexpr uint32_t kVddCalLogMagic = 0x56444443u;  // "VDDC"
constexpr uint32_t kVddCalLogVersion = 1u;
constexpr uint32_t kVddCalLogCapacity = 16u;

struct VddCalibrationRecord {
    uint32_t sequence;
    uint32_t requested_samples;
    uint32_t valid_samples;
    uint32_t code_mean;
    uint32_t code_min;
    uint32_t code_max;
    uint32_t nominal_vdd_mv;
    uint32_t adc_mode_after;
    uint32_t battload_register;
};

struct VddCalibrationLog {
    uint32_t magic;
    uint32_t version;
    uint32_t record_count;
    uint32_t next_sequence;
    VddCalibrationRecord records[kVddCalLogCapacity];
    uint32_t checksum;
};

// Volatile TCM only. The app-local linker marks this section NOLOAD so a
// debugger-induced reset does not destroy samples captured without J-Link.
// It is never programmed to MRAM and is not valid after board power is lost.
__attribute__((section(".bisen_vdd_cal_retained"), aligned(8), used))
VddCalibrationLog g_vdd_cal_log;

uint32_t vdd_cal_log_checksum(const VddCalibrationLog &log) {
    const uint32_t *words = reinterpret_cast<const uint32_t *>(&log);
    const uint32_t word_count =
        (sizeof(VddCalibrationLog) - sizeof(log.checksum)) / sizeof(uint32_t);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0u; i < word_count; ++i) {
        hash ^= words[i];
        hash *= 16777619u;
    }
    return hash;
}

bool vdd_cal_log_valid() {
    return g_vdd_cal_log.magic == kVddCalLogMagic &&
           g_vdd_cal_log.version == kVddCalLogVersion &&
           g_vdd_cal_log.record_count <= kVddCalLogCapacity &&
           g_vdd_cal_log.next_sequence != 0u &&
           g_vdd_cal_log.checksum == vdd_cal_log_checksum(g_vdd_cal_log);
}

void vdd_cal_log_prepare() {
    if (vdd_cal_log_valid()) return;
    memset(&g_vdd_cal_log, 0, sizeof(g_vdd_cal_log));
    g_vdd_cal_log.magic = kVddCalLogMagic;
    g_vdd_cal_log.version = kVddCalLogVersion;
    g_vdd_cal_log.next_sequence = 1u;
    g_vdd_cal_log.checksum = vdd_cal_log_checksum(g_vdd_cal_log);
}

void vdd_cal_log_append(const VddCalibrationRecord &record) {
    uint32_t index = g_vdd_cal_log.record_count;
    if (index >= kVddCalLogCapacity) {
        memmove(&g_vdd_cal_log.records[0], &g_vdd_cal_log.records[1],
                (kVddCalLogCapacity - 1u) * sizeof(VddCalibrationRecord));
        index = kVddCalLogCapacity - 1u;
    } else {
        ++g_vdd_cal_log.record_count;
    }
    g_vdd_cal_log.records[index] = record;
    g_vdd_cal_log.next_sequence = record.sequence + 1u;
    if (g_vdd_cal_log.next_sequence == 0u) g_vdd_cal_log.next_sequence = 1u;
    g_vdd_cal_log.checksum = vdd_cal_log_checksum(g_vdd_cal_log);
}

void vdd_cal_log_print() {
    if (!vdd_cal_log_valid()) {
        am_util_stdio_printf(
            "BISen camera VDD SRAM log INVALID; no results available\n");
        return;
    }
    am_util_stdio_printf(
        "BISen camera VDD SRAM log: records=%lu capacity=%lu"
        " volatile_only=1 MRAM_writes=0\n",
        (unsigned long)g_vdd_cal_log.record_count,
        (unsigned long)kVddCalLogCapacity);
    for (uint32_t i = 0u; i < g_vdd_cal_log.record_count; ++i) {
        const VddCalibrationRecord &record = g_vdd_cal_log.records[i];
        am_util_stdio_printf(
            "BISen camera VDD retained point #%lu: samples=%lu valid=%lu"
            " code_mean=%lu code_min=%lu code_max=%lu nominal_VDD=%lu mV"
            " ADC_mode_after=%lu MCUCTRL_ADCBATTLOAD=0x%08lx\n",
            (unsigned long)record.sequence,
            (unsigned long)record.requested_samples,
            (unsigned long)record.valid_samples,
            (unsigned long)record.code_mean,
            (unsigned long)record.code_min,
            (unsigned long)record.code_max,
            (unsigned long)record.nominal_vdd_mv,
            (unsigned long)record.adc_mode_after,
            (unsigned long)record.battload_register);
    }
}
#endif

const char *phase_name(wl_phase_t phase) {
    switch (phase) {
        case WL_PHASE_SCAN: return "camera-scan";
        case WL_PHASE_INFER: return "cnn-inference";
        case WL_PHASE_IDLE: return "idle";
    }
    return "unknown";
}

void board_init() {
    ns_core_config_t core = {.api = &ns_core_V1_0_0};
    if (ns_core_init(&core) != NS_STATUS_SUCCESS) {
        while (true) {}
    }
    if (ns_power_config(&ns_development_default) != NS_STATUS_SUCCESS) {
        while (true) {}
    }
    ns_itm_printf_enable();

    // The supplied CNN implementation was timed and verified at this mode.
    // Frequency scaling is a later, measured optimization—not assumed here.
    (void)am_hal_pwrctrl_mcu_mode_select(
        AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE);

    am_hal_stimer_config(AM_HAL_STIMER_CFG_CLEAR |
                         AM_HAL_STIMER_CFG_FREEZE);
    am_hal_stimer_config(AM_HAL_STIMER_HFRC_6MHZ);
    am_hal_sysctrl_fpu_enable();
    am_hal_sysctrl_fpu_stacking_enable(true);

    sensor_init();
    led_init();
}

void button_init() {
    ns_button_config_t config = {
        .api = &ns_button_V1_0_0,
        .button_0_enable = true,
        .button_1_enable = BISEN_CAMERA_VDD_CALIBRATION_MODE != 0,
        .joulescope_trigger_enable = false,
        .button_0_flag = &g_button0_pressed,
        .button_1_flag = &g_button1_pressed,
        .joulescope_trigger_flag = nullptr,
    };
    if (ns_peripheral_button_init(&config) != NS_STATUS_SUCCESS) {
        am_util_stdio_printf("BISen camera BTN0 init FAILED\n");
    }
    g_button0_pressed = 0;
    g_button1_pressed = 0;
}

void begin_job_tracking(bool restored) {
    ++g_job_generation;
    if (g_job_generation == 0u) ++g_job_generation;
    g_runtime_generation = restored ? 1u : 0u;
    g_committed_generation = g_runtime_generation;
    g_job_has_durable_checkpoint = restored;
    g_checkpoint_edge_armed = false;
    g_job_tracking_active = true;
}

void note_coherent_progress() {
    ++g_runtime_generation;
    if (g_runtime_generation == 0u) ++g_runtime_generation;
}

bool checkpoint_needed(const wl_state_t &state) {
    return state.dirty && state.phase != WL_PHASE_IDLE &&
           g_runtime_generation != g_committed_generation;
}

bool save_live_checkpoint() {
    wl_state_t state = {};
    wl_state(&state);
    if (!checkpoint_needed(state)) {
        return true;
    }
    const bool session_limit_reached =
        BISEN_CAMERA_MAX_CHECKPOINTS != 0u &&
        g_checkpoint_attempts >= BISEN_CAMERA_MAX_CHECKPOINTS;
    if (g_checkpoint_failure_latched || session_limit_reached) {
        g_checkpoint_failure_latched = true;
        pp_note_checkpoint_failed();
        pp_mark_boot();
        am_util_stdio_printf(
            "BISen camera checkpoint BLOCKED: attempts=%u limit=%u failure=%u;"
            " refusing to advance unprotected work\n\n",
            (unsigned)g_checkpoint_attempts,
            (unsigned)BISEN_CAMERA_MAX_CHECKPOINTS,
            g_checkpoint_failure_latched ? 1u : 0u);
        return false;
    }

    g_checkpoint_attempts++;
    pp_mark_nvm_write();
    int status = -1;
    if (state.phase == WL_PHASE_SCAN) {
        status = scan_save_mram() ? 0 : -1;
    } else if (state.phase == WL_PHASE_INFER) {
        status = ckpt_save(infer_ctx());
    }

    if (status != 0) {
        g_checkpoint_failure_latched = true;
        pp_note_checkpoint_failed();
        pp_mark_boot();
        am_util_stdio_printf(
            "BISen camera checkpoint FAILED: phase=%s position=%lu bytes=%lu\n",
            phase_name(state.phase), (unsigned long)state.position,
            (unsigned long)state.payload_bytes);
        return false;
    }

    g_checkpoint_successes++;
    g_committed_generation = g_runtime_generation;
    g_job_has_durable_checkpoint = true;
    pp_mark_committed();
    am_util_stdio_printf(
        "BISen camera checkpoint committed: phase=%s position=%lu payload=%lu"
        " job=%lu generation=%lu backend_writes=%lu HAL_programs=%lu"
        " session=%lu limit=%s backend=%s\n",
        phase_name(state.phase), (unsigned long)state.position,
        (unsigned long)state.payload_bytes,
        (unsigned long)g_job_generation,
        (unsigned long)g_runtime_generation,
        (unsigned long)g_ckpt_dev_programs,
        (unsigned long)g_ckpt_dev_hal_program_calls,
        (unsigned long)g_checkpoint_successes,
        BISEN_CAMERA_MAX_CHECKPOINTS == 0u ? "unlimited" : "configured",
        BISEN_CAMERA_ENABLE_MRAM ? "MRAM" : "retained-RAM");
    return true;
}

bool retire_completed_checkpoint() {
    if (!g_job_has_durable_checkpoint) return true;

    pp_mark_nvm_write();
    const int status = ckpt_retire();
    if (status != 1) {
        g_checkpoint_failure_latched = true;
        pp_note_checkpoint_failed();
        pp_mark_boot();
        am_util_stdio_printf(
            "BISen camera checkpoint retirement FAILED: job=%lu status=%d;"
            " old recovery record may still be live\n",
            (unsigned long)g_job_generation, status);
        return false;
    }

    ++g_checkpoint_retirements;
    g_job_has_durable_checkpoint = false;
    g_committed_generation = g_runtime_generation;
    pp_mark_committed();
    am_util_stdio_printf(
        "BISen camera checkpoint retired: job=%lu tombstones=%lu"
        " backend_writes=%lu HAL_programs=%lu\n",
        (unsigned long)g_job_generation,
        (unsigned long)g_checkpoint_retirements,
        (unsigned long)g_ckpt_dev_programs,
        (unsigned long)g_ckpt_dev_hal_program_calls);
    return true;
}

bool restore_pending_workload() {
    // The scan and inference records share the two slots. The checkpoint
    // library resolves which record is newest and validates its CRC.
    if (ckpt_newest_is_retired()) {
        am_util_stdio_printf(
            "BISen camera newest checkpoint record is retired; starting a new job\n");
        return false;
    }

    if (ckpt_scan_pending(nullptr)) {
        pp_mark_restore();
        if (scan_restore_mram()) {
            // scan_restore_mram() may have rejected a torn newest slot and
            // fallen back to the older one. Use the position it actually
            // adopted, never the unchecked header seen before CRC validation.
            const uint16_t restored_position = scan_next_pixel();
            if (wl_restore_commit(WL_PHASE_SCAN, restored_position) != 0) {
                pp_mark_boot();
                am_util_stdio_printf(
                    "BISen camera scan checkpoint adoption rejected\n");
                return false;
            }
            am_util_stdio_printf(
                "BISen camera restored scan at pixel %u/1024\n",
                (unsigned)restored_position);
            return true;
        }
        pp_mark_boot();
        am_util_stdio_printf("BISen camera scan checkpoint rejected\n");
        return false;
    }

    if (infer_init()) {
        nn_ctx_t *context = infer_ctx();
        const uint32_t position =
            ((uint32_t)context->layer << 24) |
            ((uint32_t)context->unit & 0x00ffffffu);
        pp_mark_restore();
        if (wl_restore_commit(WL_PHASE_INFER, position) == 0) {
            am_util_stdio_printf(
                "BISen camera restored CNN at layer=%u unit=%u\n",
                (unsigned)context->layer, (unsigned)context->unit);
            return true;
        }
        pp_mark_boot();
        am_util_stdio_printf("BISen camera inference checkpoint rejected\n");
    }
    return false;
}

#if BISEN_CAMERA_VDD_CALIBRATION_MODE
VddCalibrationRecord capture_vdd_calibration_record() {
    pp_mark_adc();
    uint64_t sum = 0u;
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0u;
    uint32_t valid = 0u;

    for (uint32_t i = 0u; i < BISEN_CAMERA_VDD_CALIBRATION_SAMPLES; ++i) {
        uint32_t code = 0u;
        if (adc_shared_read_supply(&code) == 0) {
            sum += code;
            if (code < minimum) minimum = code;
            if (code > maximum) maximum = code;
            ++valid;
        }
        am_util_delay_ms(2u);
    }

    VddCalibrationRecord record = {};
    record.sequence = g_vdd_cal_log.next_sequence;
    record.requested_samples = BISEN_CAMERA_VDD_CALIBRATION_SAMPLES;
    record.valid_samples = valid;
    record.code_min = valid == 0u ? 0u : minimum;
    record.code_max = valid == 0u ? 0u : maximum;
    if (valid != 0u) {
        record.code_mean = (uint32_t)((sum + valid / 2u) / valid);
        record.nominal_vdd_mv =
            adc_shared_supply_nominal_millivolts(record.code_mean);
    }
    record.adc_mode_after = (uint32_t)adc_shared_mode();
    record.battload_register = adc_shared_battload_register();
    pp_mark_sleep();
    return record;
}

void report_captured_vdd_record(const VddCalibrationRecord &record) {
    if (record.valid_samples == 0u) {
        pp_mark_boot();
        am_util_stdio_printf(
            "BISen camera VDD retained point #%lu FAILED: no valid ADC samples\n",
            (unsigned long)record.sequence);
    } else {
        am_util_stdio_printf(
            "BISen camera VDD retained point #%lu: samples=%lu valid=%lu"
            " code_mean=%lu code_min=%lu code_max=%lu nominal_VDD=%lu mV\n",
            (unsigned long)record.sequence,
            (unsigned long)record.requested_samples,
            (unsigned long)record.valid_samples,
            (unsigned long)record.code_mean,
            (unsigned long)record.code_min,
            (unsigned long)record.code_max,
            (unsigned long)record.nominal_vdd_mv);
        am_util_stdio_printf(
            "BISen camera VDD calibration audit: ADC_mode_after=%u"
            " MCUCTRL_ADCBATTLOAD=0x%08lx"
            " (app never enables load resistor)\n",
            (unsigned)record.adc_mode_after,
            (unsigned long)record.battload_register);
        pp_mark_sleep();
    }
}

[[noreturn]] void run_vdd_calibration_diagnostic() {
    vdd_cal_log_prepare();
    am_util_stdio_printf(
        "BISen camera VDD offline-capture diagnostic ARMED\n"
        "  BTN0: capture one 32-sample point into volatile SRAM\n"
        "  BTN1: print all retained points after J-Link USB is reconnected\n"
        "  Do not press RESET between capture and readout; a reset is tolerated"
        " while VDD remains powered, but a VDD power loss invalidates SRAM.\n");
    vdd_cal_log_print();
    pp_mark_sleep();

    while (true) {
        if (g_button0_pressed) {
            am_util_delay_ms(20u);
            g_button0_pressed = 0;
            const VddCalibrationRecord record =
                capture_vdd_calibration_record();
            vdd_cal_log_append(record);
            // This output is normally lost while J-Link USB is absent. It is
            // still useful if BTN0 is pressed during a connected bench check.
            report_captured_vdd_record(record);
            pp_mark_sleep();
            continue;
        }
        if (g_button1_pressed) {
            am_util_delay_ms(20u);
            g_button1_pressed = 0;
            vdd_cal_log_print();
            pp_mark_sleep();
            continue;
        }
        __WFI();
    }
}
#endif

void emit_result() {
    const int digit = wl_result();
    const int8_t *scores = infer_scores();
    int probabilities[10] = {};
    infer_probs_per_mille(probabilities);

    am_util_stdio_printf("BISen camera CNN complete: digit=%d scores=", digit);
    for (int i = 0; i < 10; ++i) {
        am_util_stdio_printf("%d%s", (int)scores[i], i == 9 ? "\n" : ",");
    }
    am_util_stdio_printf("BISen camera probabilities/1000=");
    for (int i = 0; i < 10; ++i) {
        am_util_stdio_printf("%d%s", probabilities[i], i == 9 ? "\n" : ",");
    }
    am_util_stdio_printf(
        "BISen camera timing: scan=%lu us inference=%lu us; checkpoints=%lu/%lu"
        " retirements=%lu backend_writes=%lu HAL_programs=%lu/%lu"
        " program_units_16B=%lu bytes=%lu\n",
        (unsigned long)wl_phase_last_us(WL_PHASE_SCAN),
        (unsigned long)wl_phase_last_us(WL_PHASE_INFER),
        (unsigned long)g_checkpoint_successes,
        (unsigned long)g_checkpoint_attempts,
        (unsigned long)g_checkpoint_retirements,
        (unsigned long)g_ckpt_dev_programs,
        (unsigned long)g_ckpt_dev_hal_program_successes,
        (unsigned long)g_ckpt_dev_hal_program_calls,
        (unsigned long)g_ckpt_dev_program_units,
        (unsigned long)g_ckpt_dev_bytes);
#if ES_SOURCE == ES_SOURCE_VCAP
    am_util_stdio_printf(
        "BISen camera VCAP high-sample filter: ignored=%lu"
        " retry_exhaustions=%lu cutoff=%u mV\n",
        (unsigned long)pp_high_samples_ignored(),
        (unsigned long)pp_high_retry_exhaustions(),
        (unsigned)BISEN_CAMERA_VCAP_IGNORE_AT_MV);
#endif
}

bool wait_for_energy(bool restore_threshold_required) {
    for (uint32_t wait = 0u; wait < BISEN_CAMERA_MAX_WAIT_CYCLES; ++wait) {
        pp_enter_wait();
        pwr_wait_us(BISEN_CAMERA_WAIT_US);
        pp_leave_wait();
        pp_sample();
        if (pp_compute_allowed() &&
            (!restore_threshold_required || pp_restore_allowed())) {
            am_util_stdio_printf(
                "BISen camera energy wait ended: polls=%lu code=%lu"
                " nominal_VDD=%lu mV band=%s\n",
                (unsigned long)(wait + 1u),
                (unsigned long)pp_adc_code(),
                (unsigned long)pp_vcap_mv(), pp_band_name());
            return true;
        }
    }
    am_util_stdio_printf(
        "BISen camera bounded energy wait expired: code=%lu"
        " nominal_VDD=%lu mV band=%s\n",
        (unsigned long)pp_adc_code(),
        (unsigned long)pp_vcap_mv(), pp_band_name());
    return false;
}

bool run_bisen_job(bool restored_from_storage) {
    bool require_restore_threshold = restored_from_storage;
    bool reported_initial_energy = false;
    uint32_t scheduler_steps = 0u;
    pp_high_sample_filter_reset();
    pwr_frame_start();

    for (;;) {
        pp_sample();
        if (!reported_initial_energy) {
            am_util_stdio_printf(
                "BISen camera initial energy: code=%lu nominal_VDD=%lu mV"
                " band=%s\n",
                (unsigned long)pp_adc_code(),
                (unsigned long)pp_vcap_mv(), pp_band_name());
            reported_initial_energy = true;
        }
        const bool energy_allows_work =
            pp_compute_allowed() &&
            (!require_restore_threshold || pp_restore_allowed());

        if (!energy_allows_work) {
            const bool crossed_from_work = g_checkpoint_edge_armed;
            g_checkpoint_edge_armed = false;
            wl_request_stop();
            (void)wl_step(1u);  // observe STOPPED at a coherent unit boundary

            wl_state_t state = {};
            wl_state(&state);
            const bool dirty = checkpoint_needed(state);
            if (crossed_from_work && pp_should_checkpoint() && dirty &&
                !save_live_checkpoint()) {
                return false;
            }

            am_util_stdio_printf(
                "BISen camera wait: code=%lu nominal_VDD=%lu mV band=%s"
                " phase=%s position=%lu"
                " live=%u dirty=%u job=%lu generation=%lu/%lu edge=%u\n",
                (unsigned long)pp_adc_code(),
                (unsigned long)pp_vcap_mv(), pp_band_name(),
                phase_name(state.phase), (unsigned long)state.position,
                (unsigned)state.dirty, dirty ? 1u : 0u,
                (unsigned long)g_job_generation,
                (unsigned long)g_runtime_generation,
                (unsigned long)g_committed_generation,
                crossed_from_work ? 1u : 0u);
            if (!wait_for_energy(require_restore_threshold)) {
                return false;
            }
            require_restore_threshold = false;
            wl_resume();
            continue;
        }

        require_restore_threshold = false;
        g_checkpoint_edge_armed = true;
        if (wl_stop_requested()) wl_resume();
        uint32_t chunk = pp_chunk_units();
        if (chunk == 0u) chunk = 1u;

        const wl_phase_t before = wl_current_phase();
        // One scan unit is a complete photodiode pixel and is about 1,300x
        // slower than one CNN unit in the supplied workload. Keep the direct
        // BISen band budgets for inference, but never leave VDD unobserved
        // across hundreds of camera pixels. One pixel is the smallest coherent
        // stop/checkpoint boundary exposed by the workload.
        if ((before == WL_PHASE_IDLE || before == WL_PHASE_SCAN) &&
            chunk > BISEN_CAMERA_SCAN_MAX_UNITS) {
            chunk = BISEN_CAMERA_SCAN_MAX_UNITS;
        }
        if (before == WL_PHASE_INFER ||
            (before == WL_PHASE_SCAN && scan_complete())) {
            pp_mark_compute();
        } else {
            pp_mark_camera();
        }

        const wl_step_result_t result = wl_step(chunk);
        scheduler_steps++;
        if (result == WL_STEP_COMPLETE) {
            emit_result();
            // The result payload is not persisted. If this job ever created or
            // restored a recovery checkpoint, one atomic header-only tombstone
            // retires it so a later cold boot cannot resurrect completed work.
            if (!retire_completed_checkpoint()) return false;
            wl_reset();
            g_job_tracking_active = false;
            return true;
        }
        if (result == WL_STEP_PROGRESS) note_coherent_progress();
        if (result == WL_STEP_ERROR) {
            pp_mark_boot();
            am_util_stdio_printf("BISen camera workload ERROR\n");
            return false;
        }
        if (result == WL_STEP_STOPPED) {
            continue;
        }
        if (scheduler_steps > 20000u) {
            pp_mark_boot();
            am_util_stdio_printf("BISen camera scheduler bound exceeded\n");
            return false;
        }
    }
}

void park_until_continuous_request() {
    pp_mark_sleep();
    led_select(-1);
    am_util_stdio_printf(
        "BISen camera parked safely; press BTN0 (AMAP4PEVB GPIO18)"
        " to enter continuous mode\n");
    while (!g_button0_pressed) {
        __WFI();
    }
    am_util_delay_ms(20);
    g_button0_pressed = 0;
}

}  // namespace

int main() {
    board_init();
    button_init();
    pp_instr_init();
    pp_mark_boot();

    am_util_stdio_printf(
        "\nBISen camera/CNN direct-VDD full capture:"
        " AMAP4PEVB Rev. 1 / apollo4p_evb\n");
    am_util_stdio_printf(
        "BISen camera pins: pixel=J9.10/GPIO15/ADCSE4"
        " VDD=internal BATT(VDD/3), no GPIO"
        " state_bus=J12.7/.9/.11 GPIO62,63,61"
        " BTN0=GPIO%u\n",
        (unsigned)AM_BSP_GPIO_BUTTON0);

#if BISEN_CAMERA_VDD_CALIBRATION_MODE
    am_util_stdio_printf(
        "BISen camera VDD CALIBRATION MODE: workload=off MRAM=off"
        " policy_thresholds=unset samples=%u\n",
        (unsigned)BISEN_CAMERA_VDD_CALIBRATION_SAMPLES);
    run_vdd_calibration_diagnostic();
#endif

    am_util_stdio_printf(
        "BISen camera energy=%s raw-code policy: 1000@%u (~2.20 V DMM)"
        " 500@%u (~2.10 V) 100@%u (~2.00 V) wait<%u"
        " resume=%u (~2.10 V) sleep<%u (~1.90 V); hysteresis=none\n",
        es_source_name(), (unsigned)bisen::kVddChunk1000MinCode,
        (unsigned)bisen::kVddChunk500MinCode,
        (unsigned)bisen::kVddChunk100MinCode,
        (unsigned)bisen::kVddChunk100MinCode,
        (unsigned)bisen::kVddResumeCode,
        (unsigned)bisen::kVddSleepBelowCode);
    am_util_stdio_printf(
        "BISen camera direct-VDD thresholds are provisional behavior-capture"
        " anchors from this EVB; DMM/scope VDD is the physical reference\n");
    am_util_stdio_printf(
        "BISen camera checkpoint backend=%s session_limit=%u (0=unlimited);"
        " dirty=job-generation; completion=tombstone-if-needed\n",
        BISEN_CAMERA_ENABLE_MRAM ? "two-slot app-local MRAM" : "retained RAM",
        (unsigned)BISEN_CAMERA_MAX_CHECKPOINTS);
    am_util_stdio_printf(
        "BISen camera scheduler: scan_step_max=%u pixel, CNN_band_budget=100/500/1000 units\n",
        (unsigned)BISEN_CAMERA_SCAN_MAX_UNITS);
    am_util_stdio_printf(
        "BISen camera VDD estimator: internal BATT(VDD/3), discard=1 AVG16"
        " median=3 AVG16"
        " read_cost~%u us; threshold hysteresis=none\n",
        (unsigned)adc_shared_supply_cost_us());
    am_util_stdio_printf(
        "BISen camera modes: reset=one bounded job then park;"
        " BTN0=continuous jobs; reset exits continuous mode\n");

    ckpt_init();
    bool restored = restore_pending_workload();

#if WL_APITEST
    if (!restored && wl_apitest_run() != 0) {
        pp_mark_boot();
        am_util_stdio_printf("BISen camera APITEST FAILED; target parked\n");
        while (true) __WFI();
    }
#endif

    bool reset_job_pending = BISEN_CAMERA_AUTORUN != 0;
    bool continuous_mode = false;
    uint32_t continuous_job = 0u;
    while (true) {
        if (!reset_job_pending && !continuous_mode) {
            park_until_continuous_request();
            continuous_mode = true;
            continuous_job = g_job_tracking_active ? 1u : 0u;
            am_util_stdio_printf(
                "BISen camera continuous mode entered; reset to stop\n");
        }

        const bool continuing_job = g_job_tracking_active;
        if (!continuing_job) {
            begin_job_tracking(restored);
            if (continuous_mode) ++continuous_job;
        }
        if (continuous_mode) {
            am_util_stdio_printf(
                "BISen camera continuous job #%lu %s%s\n",
                (unsigned long)continuous_job,
                continuing_job ? "continue retained SRAM progress" : "start",
                restored ? " (restored progress)" : "");
        } else {
            am_util_stdio_printf(
                "BISen camera reset-bounded job %s%s\n",
                continuing_job ? "continue retained SRAM progress" : "start",
                restored ? " (restored progress)" : "");
        }
        const bool completed = run_bisen_job(restored);
        restored = false;
        if (continuous_mode) {
            am_util_stdio_printf(
                "BISen camera continuous job #%lu %s\n",
                (unsigned long)continuous_job,
                completed ? "PASS" : "INCOMPLETE");
        } else {
            am_util_stdio_printf("BISen camera reset-bounded job %s\n",
                                 completed ? "PASS" : "INCOMPLETE");
        }
        if (!completed && g_checkpoint_failure_latched) {
            am_util_stdio_printf(
                "BISen camera storage integrity fault; target parked until reset\n");
            pp_mark_boot();
            while (true) __WFI();
        }
        reset_job_pending = false;
    }
}
