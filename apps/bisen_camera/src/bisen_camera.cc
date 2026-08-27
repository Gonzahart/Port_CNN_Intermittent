// BISen camera/CNN integration for AMAP4PEVB Rev. 1 / apollo4p_evb.
//
// The camera package owns sensing and the resumable LeNet engine. This file
// owns the energy decision, state transitions, and when recovery state is
// written. apps/bisen_port is intentionally not linked or modified.
#include <stdint.h>
#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "am_util.h"
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
#define BISEN_CAMERA_MAX_CHECKPOINTS 4
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

namespace {

volatile int g_button0_pressed;
uint32_t g_checkpoint_attempts;
uint32_t g_checkpoint_successes;
bool g_checkpoint_failure_latched;
bool g_last_saved_valid;
wl_phase_t g_last_saved_phase;
uint32_t g_last_saved_position;

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
        .button_1_enable = false,
        .joulescope_trigger_enable = false,
        .button_0_flag = &g_button0_pressed,
        .button_1_flag = nullptr,
        .joulescope_trigger_flag = nullptr,
    };
    if (ns_peripheral_button_init(&config) != NS_STATUS_SUCCESS) {
        am_util_stdio_printf("BISen camera BTN0 init FAILED\n");
    }
    g_button0_pressed = 0;
}

bool same_as_last_saved(const wl_state_t &state) {
    return g_last_saved_valid && state.phase == g_last_saved_phase &&
           state.position == g_last_saved_position;
}

void remember_saved(const wl_state_t &state) {
    g_last_saved_valid = true;
    g_last_saved_phase = state.phase;
    g_last_saved_position = state.position;
}

bool save_live_checkpoint() {
    wl_state_t state = {};
    wl_state(&state);
    if (!state.dirty || state.phase == WL_PHASE_IDLE ||
        same_as_last_saved(state)) {
        return true;
    }
    if (g_checkpoint_failure_latched ||
        g_checkpoint_attempts >= BISEN_CAMERA_MAX_CHECKPOINTS) {
        am_util_stdio_printf(
            "BISen camera checkpoint suppressed: attempts=%u limit=%u failure=%u\n\n",
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
    remember_saved(state);
    pp_mark_committed();
    am_util_stdio_printf(
        "BISen camera checkpoint committed: phase=%s position=%lu payload=%lu"
        " slot_program_calls=%lu session=%lu/%u backend=%s\n",
        phase_name(state.phase), (unsigned long)state.position,
        (unsigned long)state.payload_bytes,
        (unsigned long)g_ckpt_dev_programs,
        (unsigned long)g_checkpoint_successes,
        (unsigned)BISEN_CAMERA_MAX_CHECKPOINTS,
        BISEN_CAMERA_ENABLE_MRAM ? "MRAM" : "retained-RAM");
    return true;
}

bool restore_pending_workload() {
    // The scan and inference records share the two slots. The checkpoint
    // library resolves which record is newest and validates its CRC.
    uint16_t scan_position = 0u;
    if (ckpt_scan_pending(&scan_position)) {
        pp_mark_restore();
        if (scan_restore_mram() &&
            wl_restore_commit(WL_PHASE_SCAN, scan_position) == 0) {
            wl_state_t state = {};
            wl_state(&state);
            remember_saved(state);
            am_util_stdio_printf(
                "BISen camera restored scan at pixel %u/1024\n",
                (unsigned)scan_position);
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
            wl_state_t state = {};
            wl_state(&state);
            remember_saved(state);
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
        " MRAM_program_calls=%lu bytes=%lu\n",
        (unsigned long)wl_phase_last_us(WL_PHASE_SCAN),
        (unsigned long)wl_phase_last_us(WL_PHASE_INFER),
        (unsigned long)g_checkpoint_successes,
        (unsigned long)g_checkpoint_attempts,
        (unsigned long)g_ckpt_dev_programs,
        (unsigned long)g_ckpt_dev_bytes);
    am_util_stdio_printf(
        "BISen camera VCAP high-sample filter: ignored=%lu"
        " retry_exhaustions=%lu cutoff=%u mV\n",
        (unsigned long)pp_high_samples_ignored(),
        (unsigned long)pp_high_retry_exhaustions(),
        (unsigned)BISEN_CAMERA_VCAP_IGNORE_AT_MV);
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
                "BISen camera energy wait ended: polls=%lu VCAP=%lu mV band=%s\n",
                (unsigned long)(wait + 1u),
                (unsigned long)pp_vcap_mv(), pp_band_name());
            return true;
        }
    }
    am_util_stdio_printf(
        "BISen camera bounded energy wait expired: VCAP=%lu mV band=%s\n",
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
                "BISen camera initial energy: code=%lu VCAP=%lu mV band=%s\n",
                (unsigned long)pp_adc_code(),
                (unsigned long)pp_vcap_mv(), pp_band_name());
            reported_initial_energy = true;
        }
        const bool energy_allows_work =
            pp_compute_allowed() &&
            (!require_restore_threshold || pp_restore_allowed());

        if (!energy_allows_work) {
            wl_request_stop();
            (void)wl_step(1u);  // observe STOPPED at a coherent unit boundary

            wl_state_t state = {};
            wl_state(&state);
            if (pp_should_checkpoint() && state.dirty &&
                !same_as_last_saved(state)) {
                (void)save_live_checkpoint();
            }

            am_util_stdio_printf(
                "BISen camera wait: VCAP=%lu mV band=%s phase=%s position=%lu"
                " dirty=%u\n",
                (unsigned long)pp_vcap_mv(), pp_band_name(),
                phase_name(state.phase), (unsigned long)state.position,
                (unsigned)state.dirty);
            if (!wait_for_energy(require_restore_threshold)) {
                return false;
            }
            require_restore_threshold = false;
            wl_resume();
            continue;
        }

        require_restore_threshold = false;
        uint32_t chunk = pp_chunk_units();
        if (chunk == 0u) chunk = 1u;

        const wl_phase_t before = wl_current_phase();
        // One scan unit is a complete photodiode pixel and is about 1,300x
        // slower than one CNN unit in the supplied workload. Keep the direct
        // BISen band budgets for inference, but never leave VCAP unobserved
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
            // Completion is not an MRAM checkpoint. The old recovery record is
            // deliberately left untouched; the result is at-least-once after
            // a later cold boot until completion logging is separately agreed.
            wl_reset();
            return true;
        }
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

    am_util_stdio_printf("\nBISen camera/CNN app: AMAP4PEVB Rev. 1 / apollo4p_evb\n");
    am_util_stdio_printf(
        "BISen camera pins: pixel=J9.10/GPIO15/ADCSE4"
        " VCAP=J9.8/GPIO16/ADCSE3 state_bus=J12.7/.9/.11 GPIO62,63,61"
        " BTN0=GPIO%u\n",
        (unsigned)AM_BSP_GPIO_BUTTON0);
    am_util_stdio_printf(
        "BISen camera energy=%s thresholds: 1000@8500 500@6400 100@5900"
        " wait<5900 resume=6100 sleep<5500 mV; hysteresis=none\n",
        es_source_name());
    am_util_stdio_printf(
        "BISen camera checkpoint backend=%s session_limit=%u; completion writes=off\n",
        BISEN_CAMERA_ENABLE_MRAM ? "two-slot app-local MRAM" : "retained RAM",
        (unsigned)BISEN_CAMERA_MAX_CHECKPOINTS);
    am_util_stdio_printf(
        "BISen camera scheduler: scan_step_max=%u pixel, CNN_band_budget=100/500/1000 units\n",
        (unsigned)BISEN_CAMERA_SCAN_MAX_UNITS);
    am_util_stdio_printf(
        "BISen camera VCAP high-sample filter: ignore >=%u mV,"
        " immediate_retries=%u; exhausted retries enter wait\n",
        (unsigned)BISEN_CAMERA_VCAP_IGNORE_AT_MV,
        (unsigned)BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES);
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
            continuous_job = 0u;
            am_util_stdio_printf(
                "BISen camera continuous mode entered; reset to stop\n");
        }

        if (continuous_mode) {
            ++continuous_job;
            am_util_stdio_printf(
                "BISen camera continuous job #%lu start%s\n",
                (unsigned long)continuous_job,
                restored ? " (restored progress)" : "");
        } else {
            am_util_stdio_printf(
                "BISen camera reset-bounded job start%s\n",
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
        reset_job_pending = false;
    }
}
