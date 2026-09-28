// BISen camera/CNN external-trace integration for AMAP4PEVB Rev. 1 / apollo4p_evb.
//
// The camera package owns sensing and the resumable LeNet engine. This file
// owns the energy decision, state transitions, and when recovery state is
// written. apps/bisen_port is intentionally not linked or modified.
#include <stdint.h>
#include <string.h>
#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "am_util.h"
#include "log_control.h"
#include "adc_shared.h"
#include "bisen/bisen_config.h"
#include "ckpt.h"
#include "energy_source.h"
#include "harvest_stats.h"
#include "infer.h"
#include "ns_ambiqsuite_harness.h"
#include "ns_core.h"
#include "ns_peripherals_button.h"
#include "ns_peripherals_power.h"
#include "power.h"
#include "power_policy.h"
#include "scan.h"
#include "sensor.h"
#include "trace_input.h"
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
#define BISEN_CAMERA_MAX_WAIT_CYCLES 0u
#endif
#ifndef BISEN_CAMERA_AUTORUN
#define BISEN_CAMERA_AUTORUN 1
#endif
#ifndef BISEN_HARVEST_AUTOCONTINUOUS
#define BISEN_HARVEST_AUTOCONTINUOUS 0
#endif
#ifndef BISEN_HARVEST_OFFLINE_VALIDATE
#define BISEN_HARVEST_OFFLINE_VALIDATE 0
#endif
#if BISEN_HARVEST_OFFLINE_VALIDATE && BISEN_CAMERA_AUTORUN
#error "Offline validation requires BISEN_CAMERA_AUTORUN=0 so a debugger reset cannot overwrite the retained result"
#endif
#if BISEN_HARVEST_OFFLINE_VALIDATE && BISEN_HARVEST_AUTOCONTINUOUS
#error "Offline validation and automatic continuous replay are mutually exclusive"
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
#ifndef BISEN_TRACE_CALIBRATION_MODE
#define BISEN_TRACE_CALIBRATION_MODE 1
#endif
#ifndef BISEN_TRACE_CALIBRATION_SAMPLES
#define BISEN_TRACE_CALIBRATION_SAMPLES 32u
#endif
#if BISEN_TRACE_CALIBRATION_SAMPLES < 2
#error "VDD calibration needs at least two ADC samples per DMM setpoint"
#endif
#if BISEN_TRACE_CALIBRATION_MODE && BISEN_CAMERA_ENABLE_MRAM
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
bool g_user_stop_requested;
bool g_user_stop_completed;

#if BISEN_HARVEST_OFFLINE_VALIDATE
constexpr uint32_t kOfflineMagic = 0x48564C31u;  // "HVL1"
constexpr uint32_t kOfflineVersion = 4u;

struct OfflineValidationLog {
    uint32_t magic;
    uint32_t version;
    uint32_t boots;
    uint32_t trial;
    uint32_t status;  // 0=no trial, 1=running, 2=PASS, 3=INCOMPLETE
    uint32_t wait_count;
    uint32_t resume_count;
    uint32_t checkpoint_count;
    uint32_t storage_restore_count;
    uint32_t first_storage_restore_phase;
    uint32_t first_storage_restore_position;
    uint32_t first_wait_phase;
    uint32_t first_wait_position;
    uint32_t max_wait_position;
    uint32_t last_wait_phase;
    uint32_t first_checkpoint_phase;
    uint32_t first_checkpoint_position;
    uint32_t last_checkpoint_phase;
    uint32_t last_checkpoint_position;
    uint32_t first_wait_vcap_mv;
    uint32_t last_wait_vcap_mv;
    uint32_t first_resume_vcap_mv;
    uint32_t last_resume_vcap_mv;
    uint32_t completed_results;
    uint32_t last_digit;
    uint32_t backend_programs;
    uint32_t checksum;
};

// NOLOAD TCM survives a debugger-induced reset while VDD remains present.
// It cannot survive actual board power loss and is never a durable checkpoint.
__attribute__((section(".bisen_vdd_cal_retained"), aligned(8), used))
OfflineValidationLog g_offline_validation;

// restore_pending_workload() runs before offline_begin(). Keep the recovery
// evidence in ordinary boot-local RAM until BTN0 begins the post-boot job, at
// which point it is copied into the retained summary. This lets a cold-restore
// trial report the exact phase/position recovered from MRAM after J-Link is
// reconnected, without mistaking an older retained summary for this boot.
uint32_t g_boot_storage_restore_count;
uint32_t g_boot_storage_restore_phase;
uint32_t g_boot_storage_restore_position;

uint32_t offline_checksum(const OfflineValidationLog &log) {
    const uint32_t *words = reinterpret_cast<const uint32_t *>(&log);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < (sizeof(log) / sizeof(uint32_t)) - 1u; ++i) {
        hash ^= words[i];
        hash *= 16777619u;
    }
    return hash;
}

void offline_touch() {
    g_offline_validation.checksum = offline_checksum(g_offline_validation);
}

bool offline_valid() {
    return g_offline_validation.magic == kOfflineMagic &&
           g_offline_validation.version == kOfflineVersion &&
           g_offline_validation.checksum == offline_checksum(g_offline_validation);
}

void offline_prepare() {
    if (!offline_valid()) {
        memset(&g_offline_validation, 0, sizeof(g_offline_validation));
        g_offline_validation.magic = kOfflineMagic;
        g_offline_validation.version = kOfflineVersion;
    }
    ++g_offline_validation.boots;
    offline_touch();
}

void offline_begin() {
    const uint32_t boots = g_offline_validation.boots;
    const uint32_t trial = g_offline_validation.trial + 1u;
    memset(&g_offline_validation, 0, sizeof(g_offline_validation));
    g_offline_validation.magic = kOfflineMagic;
    g_offline_validation.version = kOfflineVersion;
    g_offline_validation.boots = boots;
    g_offline_validation.trial = trial;
    g_offline_validation.status = 1u;
    g_offline_validation.storage_restore_count =
        g_boot_storage_restore_count;
    g_offline_validation.first_storage_restore_phase =
        g_boot_storage_restore_phase;
    g_offline_validation.first_storage_restore_position =
        g_boot_storage_restore_position;
    // Recovery evidence belongs to the first job started after this boot.
    // Consume it here so a later diagnostic job cannot claim the same MRAM
    // restore. Offline validation normally latches after that first job, but
    // this also keeps the retained record correct if that policy is changed.
    g_boot_storage_restore_count = 0u;
    g_boot_storage_restore_phase = 0u;
    g_boot_storage_restore_position = 0u;
    offline_touch();
}

void offline_note_storage_restore(wl_phase_t phase, uint32_t position) {
    ++g_boot_storage_restore_count;
    if (g_boot_storage_restore_count == 1u) {
        g_boot_storage_restore_phase = static_cast<uint32_t>(phase);
        g_boot_storage_restore_position = position;
    }
}

void offline_note_wait(wl_phase_t phase, uint32_t position, uint32_t vcap_mv) {
    if (g_offline_validation.wait_count == 0u) {
        g_offline_validation.first_wait_phase = static_cast<uint32_t>(phase);
        g_offline_validation.first_wait_position = position;
        g_offline_validation.first_wait_vcap_mv = vcap_mv;
    }
    ++g_offline_validation.wait_count;
    if (position > g_offline_validation.max_wait_position)
        g_offline_validation.max_wait_position = position;
    g_offline_validation.last_wait_phase = static_cast<uint32_t>(phase);
    g_offline_validation.last_wait_vcap_mv = vcap_mv;
    offline_touch();
}

void offline_note_resume(uint32_t vcap_mv) {
    if (g_offline_validation.resume_count == 0u)
        g_offline_validation.first_resume_vcap_mv = vcap_mv;
    ++g_offline_validation.resume_count;
    g_offline_validation.last_resume_vcap_mv = vcap_mv;
    offline_touch();
}

void offline_note_checkpoint(const wl_state_t &state) {
    if (g_offline_validation.checkpoint_count == 0u) {
        g_offline_validation.first_checkpoint_phase =
            static_cast<uint32_t>(state.phase);
        g_offline_validation.first_checkpoint_position = state.position;
    }
    ++g_offline_validation.checkpoint_count;
    g_offline_validation.last_checkpoint_phase =
        static_cast<uint32_t>(state.phase);
    g_offline_validation.last_checkpoint_position = state.position;
    offline_touch();
}

void offline_note_result(int digit) {
    ++g_offline_validation.completed_results;
    g_offline_validation.last_digit = static_cast<uint32_t>(digit);
    offline_touch();
}

void offline_finish(bool passed, uint32_t backend_programs) {
    g_offline_validation.status = passed ? 2u : 3u;
    g_offline_validation.backend_programs = backend_programs;
    offline_touch();
}

void offline_print() {
    if (!offline_valid()) {
        am_util_stdio_printf("HARVEST offline validation INVALID (SRAM lost or incomplete)\n");
        return;
    }
    const OfflineValidationLog &log = g_offline_validation;
    am_util_stdio_printf(
        "HARVEST offline validation: trial=%lu boots=%lu status=%lu"
        " waits=%lu first_wait_phase=%lu first_wait_position=%lu"
        " max_wait_position=%lu last_wait_phase=%lu"
        " first_wait_mV=%lu last_wait_mV=%lu\n",
        (unsigned long)log.trial, (unsigned long)log.boots,
        (unsigned long)log.status, (unsigned long)log.wait_count,
        (unsigned long)log.first_wait_phase,
        (unsigned long)log.first_wait_position,
        (unsigned long)log.max_wait_position,
        (unsigned long)log.last_wait_phase,
        (unsigned long)log.first_wait_vcap_mv,
        (unsigned long)log.last_wait_vcap_mv);
    am_util_stdio_printf(
        "HARVEST offline validation: resumes=%lu first_resume_mV=%lu"
        " last_resume_mV=%lu results=%lu last_digit=%ld"
        " commits=%lu backend=%s backend_programs=%lu\n",
        (unsigned long)log.resume_count,
        (unsigned long)log.first_resume_vcap_mv,
        (unsigned long)log.last_resume_vcap_mv,
        (unsigned long)log.completed_results,
        (long)static_cast<int32_t>(log.last_digit),
        (unsigned long)log.checkpoint_count,
        BISEN_CAMERA_ENABLE_MRAM ? "MRAM" : "retained-RAM",
        (unsigned long)log.backend_programs);
    am_util_stdio_printf(
        "HARVEST offline validation: first_checkpoint_phase=%lu"
        " first_checkpoint_position=%lu last_checkpoint_phase=%lu"
        " last_checkpoint_position=%lu\n",
        (unsigned long)log.first_checkpoint_phase,
        (unsigned long)log.first_checkpoint_position,
        (unsigned long)log.last_checkpoint_phase,
        (unsigned long)log.last_checkpoint_position);
    am_util_stdio_printf(
        "HARVEST offline validation: storage_restores=%lu"
        " first_storage_restore_phase=%lu"
        " first_storage_restore_position=%lu\n",
        (unsigned long)log.storage_restore_count,
        (unsigned long)log.first_storage_restore_phase,
        (unsigned long)log.first_storage_restore_position);
}
#endif

#if BISEN_TRACE_CALIBRATION_MODE
constexpr uint32_t kVddCalLogMagic = 0x48564343u;  // "HVCC", separate from trace calibration
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
            "BISen camera HARVEST SRAM log INVALID; no results available\n");
        return;
    }
    am_util_stdio_printf(
        "BISen camera HARVEST SRAM log: records=%lu capacity=%lu"
        " volatile_only=1 MRAM_writes=0\n",
        (unsigned long)g_vdd_cal_log.record_count,
        (unsigned long)kVddCalLogCapacity);
    for (uint32_t i = 0u; i < g_vdd_cal_log.record_count; ++i) {
        const VddCalibrationRecord &record = g_vdd_cal_log.records[i];
        am_util_stdio_printf(
            "BISen camera HARVEST retained point #%lu: samples=%lu valid=%lu"
            " code_mean=%lu code_min=%lu code_max=%lu VCAP_nominal_mV=%lu mV"
            " ADC_mode_after=%lu\n",
            (unsigned long)record.sequence,
            (unsigned long)record.requested_samples,
            (unsigned long)record.valid_samples,
            (unsigned long)record.code_mean,
            (unsigned long)record.code_min,
            (unsigned long)record.code_max,
            (unsigned long)record.nominal_vdd_mv,
            (unsigned long)record.adc_mode_after);
    }
}
#endif

#if BISEN_ENABLE_SWO_LOGGING
const char *phase_name(wl_phase_t phase) {
    switch (phase) {
        case WL_PHASE_SCAN: return "camera-scan";
        case WL_PHASE_INFER: return "cnn-inference";
        case WL_PHASE_IDLE: return "idle";
    }
    return "unknown";
}
#endif

void board_init() {
    ns_core_config_t core = {.api = &ns_core_V1_0_0};
    if (ns_core_init(&core) != NS_STATUS_SUCCESS) {
        while (true) {}
    }
    if (ns_power_config(&ns_development_default) != NS_STATUS_SUCCESS) {
        while (true) {}
    }
#if BISEN_ENABLE_SWO_LOGGING
    ns_itm_printf_enable();
#endif

    // Keep 192 MHz as the validated baseline. 96 MHz is an explicit energy-
    // per-completed-job experiment, never selected from current draw alone.
    (void)am_hal_pwrctrl_mcu_mode_select(
#if BISEN_HARVEST_MCU_LOW_POWER
        AM_HAL_PWRCTRL_MCU_MODE_LOW_POWER);
#else
        AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE);
#endif

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
        .button_1_enable = BISEN_TRACE_CALIBRATION_MODE != 0 ||
                           BISEN_HARVEST_OFFLINE_VALIDATE != 0 ||
                           BISEN_HARVEST_AUTOCONTINUOUS != 0,
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
#if BISEN_HARVEST_OFFLINE_VALIDATE
    offline_note_checkpoint(state);
#endif
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

#if BISEN_HARVEST_AUTOCONTINUOUS
bool set_continuous_session_armed(bool armed) {
    pp_mark_nvm_write();
    if (ckpt_session_set_armed(armed ? 1 : 0) != 0) {
        g_checkpoint_failure_latched = true;
        pp_note_checkpoint_failed();
        pp_mark_boot();
        am_util_stdio_printf(
            "BISen camera continuous-run marker FAILED: requested=%u\n",
            armed ? 1u : 0u);
        return false;
    }
    pp_mark_committed();
    return true;
}

bool consume_stop_button() {
    if (!g_button1_pressed) return false;
    am_util_delay_ms(20u);
    g_button1_pressed = 0;
    g_user_stop_requested = true;
    return true;
}

bool stop_continuous_session() {
    // Stop at the workload's next coherent unit. Persist progress first; only
    // then clear the run marker. A loss between those two commits restarts the
    // session and resumes valid progress rather than silently losing work.
    wl_request_stop();
    (void)wl_step(1u);
    g_checkpoint_edge_armed = false;
    if (!save_live_checkpoint()) return false;
    if (!set_continuous_session_armed(false)) return false;

    g_user_stop_requested = false;
    g_user_stop_completed = true;
    pp_mark_sleep();
    am_util_stdio_printf(
        "BISen camera continuous mode stopped by BTN1; progress retained;"
        " BTN0 resumes\n");
    return true;
}
#endif

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
    // A zero-filled nn_ctx_t looks like layer 0/unit 0, which
    // nn_in_progress() correctly treats as an active inference. On a cold
    // boot, however, inference has not begun yet. Mark it inactive before
    // selecting a scan or inference recovery record; ckpt_restore() will
    // overwrite the context again if a real inference checkpoint exists.
    // Without this, wl_state() can incorrectly outrank an in-progress camera
    // scan with a fictitious CNN checkpoint at phase 2, position 0.
    nn_abandon(infer_ctx());

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
#if BISEN_HARVEST_OFFLINE_VALIDATE
            offline_note_storage_restore(WL_PHASE_SCAN, restored_position);
#endif
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
#if BISEN_HARVEST_OFFLINE_VALIDATE
            offline_note_storage_restore(WL_PHASE_INFER, position);
#endif
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

#if BISEN_TRACE_CALIBRATION_MODE
VddCalibrationRecord capture_vdd_calibration_record() {
    pp_mark_adc();
    uint64_t sum = 0u;
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0u;
    uint32_t valid = 0u;

    for (uint32_t i = 0u; i < BISEN_TRACE_CALIBRATION_SAMPLES; ++i) {
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
    record.requested_samples = BISEN_TRACE_CALIBRATION_SAMPLES;
    record.valid_samples = valid;
    record.code_min = valid == 0u ? 0u : minimum;
    record.code_max = valid == 0u ? 0u : maximum;
    if (valid != 0u) {
        record.code_mean = (uint32_t)((sum + valid / 2u) / valid);
        record.nominal_vdd_mv =
            adc_shared_supply_nominal_millivolts(record.code_mean);
    }
    record.adc_mode_after = (uint32_t)adc_shared_mode();
    pp_mark_sleep();
    return record;
}

void report_captured_vdd_record(const VddCalibrationRecord &record) {
    if (record.valid_samples == 0u) {
        pp_mark_boot();
        am_util_stdio_printf(
            "BISen camera HARVEST retained point #%lu FAILED: no valid ADC samples\n",
            (unsigned long)record.sequence);
    } else {
        am_util_stdio_printf(
            "BISen camera HARVEST retained point #%lu: samples=%lu valid=%lu"
            " code_mean=%lu code_min=%lu code_max=%lu VCAP_nominal_mV=%lu mV\n",
            (unsigned long)record.sequence,
            (unsigned long)record.requested_samples,
            (unsigned long)record.valid_samples,
            (unsigned long)record.code_mean,
            (unsigned long)record.code_min,
            (unsigned long)record.code_max,
            (unsigned long)record.nominal_vdd_mv);
        am_util_stdio_printf(
            "BISen camera HARVEST calibration audit: ADC_mode_after=%u;"
            " code_mean is the physical ADCSE3 code\n",
            (unsigned)record.adc_mode_after);
        pp_mark_sleep();
    }
}

[[noreturn]] void run_vdd_calibration_diagnostic() {
    vdd_cal_log_prepare();
    am_util_stdio_printf(
        "BISen camera HARVEST offline-capture diagnostic ARMED\n"
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
#if BISEN_HARVEST_OFFLINE_VALIDATE || BISEN_ENABLE_SWO_LOGGING
    const int digit = wl_result();
#endif
#if BISEN_HARVEST_OFFLINE_VALIDATE
    offline_note_result(digit);
#endif
#if BISEN_ENABLE_SWO_LOGGING
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
    harvest_stats_t stats = {};
    harvest_stats_snapshot(&stats);
    am_util_stdio_printf(
        "HARVEST stats: state0=%lu ms camera=%lu ms CNN=%lu ms"
        " VCAP_reads=%lu camera_activations=%lu"
        " scheduled_chunks_100/500/1000=%lu/%lu/%lu"
        " MRAM_programs=%lu commits=%lu retirements=%lu restores=%lu\n",
        (unsigned long)(stats.state_ticks[0] / 6000u),
        (unsigned long)(stats.state_ticks[2] / 6000u),
        (unsigned long)(stats.state_ticks[3] / 6000u),
        (unsigned long)stats.vcap_observations,
        (unsigned long)stats.camera_activations,
        (unsigned long)stats.chunks_100,
        (unsigned long)stats.chunks_500,
        (unsigned long)stats.chunks_1000,
        (unsigned long)g_ckpt_dev_programs,
        (unsigned long)g_checkpoint_successes,
        (unsigned long)g_checkpoint_retirements,
        (unsigned long)stats.state_entries[6]);
#if ES_SOURCE == ES_SOURCE_VCAP
    am_util_stdio_printf(
        "BISen camera VCAP high-sample filter: ignored=%lu"
        " retry_exhaustions=%lu cutoff=%u mV\n",
        (unsigned long)pp_high_samples_ignored(),
        (unsigned long)pp_high_retry_exhaustions(),
        (unsigned)BISEN_CAMERA_VCAP_IGNORE_AT_MV);
#endif
#endif
}

bool wait_for_energy(bool restore_threshold_required) {
    for (uint32_t wait = 0u;
         BISEN_CAMERA_MAX_WAIT_CYCLES == 0u ||
         wait < BISEN_CAMERA_MAX_WAIT_CYCLES; ++wait) {
#if BISEN_HARVEST_AUTOCONTINUOUS
        if (consume_stop_button()) return false;
#endif
        pp_enter_wait();
        pwr_wait_us(BISEN_CAMERA_WAIT_US);
        pp_leave_wait();
#if BISEN_HARVEST_AUTOCONTINUOUS
        if (consume_stop_button()) return false;
#endif
        pp_sample();
        if (pp_compute_allowed() &&
            (!restore_threshold_required || pp_restore_allowed())) {
#if BISEN_HARVEST_OFFLINE_VALIDATE
            offline_note_resume(pp_vcap_mv());
#endif
            am_util_stdio_printf(
                "BISen camera energy wait ended: polls=%lu policy_code=%lu"
                " VCAP_mV=%lu mV band=%s\n",
                (unsigned long)(wait + 1u),
                (unsigned long)pp_adc_code(),
                (unsigned long)pp_vcap_mv(), pp_band_name());
            return true;
        }
    }
    am_util_stdio_printf(
        "BISen camera bounded energy wait expired: policy_code=%lu"
        " VCAP_mV=%lu mV band=%s\n",
        (unsigned long)pp_adc_code(),
        (unsigned long)pp_vcap_mv(), pp_band_name());
    return false;
}

bool run_bisen_job(bool restored_from_storage) {
    bool require_restore_threshold = restored_from_storage;
    bool camera_seen = false;
    bool reported_initial_energy = false;
    uint32_t scheduler_steps = 0u;
    pp_high_sample_filter_reset();
    pwr_frame_start();

    for (;;) {
        pp_sample();
#if BISEN_HARVEST_AUTOCONTINUOUS
        (void)consume_stop_button();
        if (g_user_stop_requested) {
            (void)stop_continuous_session();
            return false;
        }
#endif
        if (!reported_initial_energy) {
            am_util_stdio_printf(
                "BISen camera initial energy: policy_code=%lu VCAP_mV=%lu mV"
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
#if BISEN_HARVEST_OFFLINE_VALIDATE
            offline_note_wait(state.phase, state.position, pp_vcap_mv());
#endif
            const bool dirty = checkpoint_needed(state);
            if (crossed_from_work && pp_should_checkpoint() && dirty &&
                !save_live_checkpoint()) {
                return false;
            }

            am_util_stdio_printf(
                "BISen camera wait: policy_code=%lu VCAP_mV=%lu mV band=%s"
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
            // Every low-energy stop must recover to the higher resume edge.
            // Restarting at the same work100 edge that caused the stop made
            // ADC variation generate repeated waits and logical checkpoints.
            if (!wait_for_energy(true)) {
#if BISEN_HARVEST_AUTOCONTINUOUS
                if (g_user_stop_requested) {
                    (void)stop_continuous_session();
                }
#endif
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
        if (before == WL_PHASE_SCAN && !camera_seen) {
            harvest_stats_note_camera();
            camera_seen = true;
        }
        if (before == WL_PHASE_INFER) {
            harvest_stats_note_chunk(pp_chunk_units());
        }
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
    adc_shared_park();
#if BISEN_HARVEST_OFFLINE_VALIDATE
    am_util_stdio_printf(
        "BISen camera offline validation parked; BTN0 runs one job;"
        " BTN1 prints retained result\n");
#else
    am_util_stdio_printf(
        "BISen camera parked safely; press BTN0 (AMAP4PEVB GPIO18)"
        " to enter continuous mode\n");
#endif
    while (!g_button0_pressed) {
#if BISEN_HARVEST_OFFLINE_VALIDATE
        if (g_button1_pressed) {
            am_util_delay_ms(20);
            g_button1_pressed = 0;
            offline_print();
            pp_mark_sleep();
            continue;
        }
#endif
#if BISEN_HARVEST_AUTOCONTINUOUS
        // BTN1 while already parked is intentionally idempotent. Clear a
        // release edge so it cannot immediately stop the next BTN0 session.
        if (g_button1_pressed) {
            am_util_delay_ms(20u);
            g_button1_pressed = 0;
        }
#endif
        __WFI();
    }
    am_util_delay_ms(20);
    g_button0_pressed = 0;
    g_button1_pressed = 0;
}

#if BISEN_HARVEST_OFFLINE_VALIDATE
void park_after_offline_result() {
    pp_mark_sleep();
    led_select(-1);
    adc_shared_park();
    g_button0_pressed = 0;
    am_util_stdio_printf(
        "BISen camera offline result latched; BTN1 prints it;"
        " reset or power-cycle starts another trial\n");
    while (true) {
        // Deliberately ignore BTN0 after the completed diagnostic. A release
        // edge or switch bounce must not start a second workload and replace
        // the cold-restore evidence before J-Link readout.
        g_button0_pressed = 0;
        if (g_button1_pressed) {
            am_util_delay_ms(20);
            g_button1_pressed = 0;
            offline_print();
            pp_mark_sleep();
        }
        __WFI();
    }
}
#endif

}  // namespace

int main() {
    board_init();
    button_init();
#if BISEN_HARVEST_OFFLINE_VALIDATE
    offline_prepare();
#endif
    pp_instr_init();
    pp_mark_boot();

    am_util_stdio_printf(
        "\nBISen camera/CNN PHYSICAL VCAP HARVEST:"
        " AMAP4PEVB Rev. 1 / apollo4p_evb\n");
    am_util_stdio_printf(
        "BISen camera pins: pixel=J9.10/GPIO15/ADCSE4"
        " VCAP=J9.8/GPIO16/ADCSE3 via %lu/%lu ohm divider"
        " state_bus=J12.7/.9/.11 GPIO62,63,61"
        " BTN0=GPIO%u\n",
        (unsigned long)BISEN_HARVEST_DIVIDER_TOP_OHM,
        (unsigned long)BISEN_HARVEST_DIVIDER_BOTTOM_OHM,
        (unsigned)AM_BSP_GPIO_BUTTON0);

#if BISEN_TRACE_CALIBRATION_MODE
    am_util_stdio_printf(
        "BISen camera HARVEST CALIBRATION MODE: workload=off MRAM=off"
        " policy_thresholds=unset samples=%u\n",
        (unsigned)BISEN_TRACE_CALIBRATION_SAMPLES);
    run_vdd_calibration_diagnostic();
#endif

    am_util_stdio_printf(
        "BISen camera energy=%s virtual-code policy: 1000@%u"
        " 500@%u 100@%u wait<%u resume=%u critical<%u;"
        " post-wait resume hysteresis=enabled\n",
        es_source_name(), (unsigned)bisen::kVddChunk1000MinCode,
        (unsigned)bisen::kVddChunk500MinCode,
        (unsigned)bisen::kVddChunk100MinCode,
        (unsigned)bisen::kVddChunk100MinCode,
        (unsigned)bisen::kVddResumeCode,
        (unsigned)bisen::kVddSleepBelowCode);
    am_util_stdio_printf(
        "PHYSICAL VCAP: policy_code is virtual; VCAP_mV is measured ahead"
        " of MP1584EN, NOT regulated board VDD\n");
    am_util_stdio_printf(
        "Physical VCAP thresholds: critical=%lu work100=%lu work500=%lu"
        " work1000=%lu uV; max=%lu uV; switched_divider=%u ADC_park=%u\n",
        (unsigned long)BISEN_HARVEST_CRITICAL_UV,
        (unsigned long)BISEN_HARVEST_WORK100_UV,
        (unsigned long)BISEN_HARVEST_WORK500_UV,
        (unsigned long)BISEN_HARVEST_WORK1000_UV,
        (unsigned long)BISEN_HARVEST_MAX_VCAP_UV,
        (unsigned)BISEN_HARVEST_SWITCHED_DIVIDER,
        (unsigned)BISEN_HARVEST_PARK_ADC);
    am_util_stdio_printf(
        "BISen camera checkpoint backend=%s session_limit=%u (0=unlimited);"
        " dirty=job-generation; completion=tombstone-if-needed\n",
        BISEN_CAMERA_ENABLE_MRAM ? "two-slot app-local MRAM" : "retained RAM",
        (unsigned)BISEN_CAMERA_MAX_CHECKPOINTS);
    am_util_stdio_printf(
        "BISen camera scheduler: scan_step_max=%u pixel, CNN_band_budget=100/500/1000 units\n",
        (unsigned)BISEN_CAMERA_SCAN_MAX_UNITS);
    am_util_stdio_printf(
        "BISen camera VCAP estimator: external ADCSE3, discard=1 AVG16"
        " median=3 AVG16"
        " read_cost~%u us; stop=work100 resume=work500\n",
        (unsigned)adc_shared_supply_cost_us());
#if BISEN_HARVEST_OFFLINE_VALIDATE
    am_util_stdio_printf(
        "BISen camera offline validation: AUTORUN=0 BTN0=one job"
        " BTN1=retained SRAM readout; reset required for another trial;"
        " keep target VDD present\n");
#elif BISEN_HARVEST_AUTOCONTINUOUS
    am_util_stdio_printf(
        "BISen camera RF replay mode: BTN0 arms continuous jobs;"
        " BTN1 checkpoints and parks; armed sessions auto-resume after"
        " reset or power loss\n");
#else
    am_util_stdio_printf(
        "BISen camera modes: reset=one bounded job then park;"
        " BTN0=continuous jobs; reset exits continuous mode\n");
#endif

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
    bool continuous_mode =
        BISEN_HARVEST_AUTOCONTINUOUS != 0 &&
        ckpt_session_is_armed() != 0;
    uint32_t continuous_job = 0u;
    while (true) {
        if (!reset_job_pending && !continuous_mode) {
            park_until_continuous_request();
#if BISEN_HARVEST_AUTOCONTINUOUS
            if (!set_continuous_session_armed(true)) {
                pp_mark_boot();
                while (true) __WFI();
            }
            g_user_stop_requested = false;
            g_user_stop_completed = false;
#endif
            continuous_mode = true;
            continuous_job = g_job_tracking_active ? 1u : 0u;
#if BISEN_HARVEST_OFFLINE_VALIDATE
            am_util_stdio_printf(
                "BISen camera offline single-job mode entered\n");
#else
            am_util_stdio_printf(
                "BISen camera continuous mode armed; BTN1 stops cleanly\n");
#endif
        }

        const bool continuing_job = g_job_tracking_active;
        if (!continuing_job) {
            begin_job_tracking(restored);
#if BISEN_HARVEST_OFFLINE_VALIDATE
            offline_begin();
#endif
            if (continuous_mode) ++continuous_job;
        }
        if (continuous_mode) {
#if BISEN_HARVEST_OFFLINE_VALIDATE
            am_util_stdio_printf(
                "BISen camera offline job #%lu %s%s\n",
                (unsigned long)continuous_job,
                continuing_job ? "continue retained SRAM progress" : "start",
                restored ? " (restored progress)" : "");
#else
            am_util_stdio_printf(
                "BISen camera continuous job #%lu %s%s\n",
                (unsigned long)continuous_job,
                continuing_job ? "continue retained SRAM progress" : "start",
                restored ? " (restored progress)" : "");
#endif
        } else {
            am_util_stdio_printf(
                "BISen camera reset-bounded job %s%s\n",
                continuing_job ? "continue retained SRAM progress" : "start",
                restored ? " (restored progress)" : "");
        }
        const bool completed = run_bisen_job(restored);
#if BISEN_HARVEST_OFFLINE_VALIDATE
        offline_finish(completed, g_ckpt_dev_programs);
#endif
        restored = false;
#if BISEN_HARVEST_AUTOCONTINUOUS
        if (g_user_stop_completed) {
            continuous_mode = false;
            reset_job_pending = false;
            g_user_stop_completed = false;
            am_util_stdio_printf(
                "BISen camera parked with resumable progress; BTN0 restarts"
                " continuous mode\n");
            continue;
        }
#endif
        if (continuous_mode) {
#if BISEN_HARVEST_OFFLINE_VALIDATE
            am_util_stdio_printf(
                "BISen camera offline job #%lu %s\n",
                (unsigned long)continuous_job,
                completed ? "PASS" : "INCOMPLETE");
#else
            am_util_stdio_printf(
                "BISen camera continuous job #%lu %s\n",
                (unsigned long)continuous_job,
                completed ? "PASS" : "INCOMPLETE");
#endif
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
#if BISEN_HARVEST_OFFLINE_VALIDATE
        // One boot produces one immutable validation result. This prevents a
        // stale BTN0 edge from launching another job and overwriting the
        // recovered phase/position before the user reconnects J-Link.
        park_after_offline_result();
#endif
        reset_job_pending = false;
    }
}
