#include "bisen_power.h"

#include "bisen_config.h"
#include "ns_ambiqsuite_harness.h"
#include "ns_peripherals_power.h"

namespace {

volatile bool g_rtc_alarm_fired = false;
volatile uint32_t g_rtc_wake_count = 0u;
volatile bisen::RtcFailureStage g_rtc_failure_stage =
    bisen::RtcFailureStage::kNone;
volatile uint32_t g_rtc_last_hal_status = AM_HAL_STATUS_SUCCESS;
#if BISEN_ENABLE_LOW_POWER
bool g_rtc_initialized = false;

bool rtc_hal_step(uint32_t status, bisen::RtcFailureStage stage) {
    if (status == AM_HAL_STATUS_SUCCESS) {
        return true;
    }
    g_rtc_failure_stage = stage;
    g_rtc_last_hal_status = status;
    return false;
}
#endif

}  // namespace

extern "C" void am_rtc_isr(void) {
    uint32_t status = 0u;
    const uint32_t query_status =
        am_hal_rtc_interrupt_status_get(true, &status);
    if (query_status != AM_HAL_STATUS_SUCCESS) {
        g_rtc_failure_stage = bisen::RtcFailureStage::kInterruptStatus;
        g_rtc_last_hal_status = query_status;
        return;
    }
    if ((status & AM_HAL_RTC_INT_ALM) != 0u) {
        const uint32_t clear_status =
            am_hal_rtc_interrupt_clear(AM_HAL_RTC_INT_ALM);
        if (clear_status != AM_HAL_STATUS_SUCCESS) {
            g_rtc_failure_stage =
                bisen::RtcFailureStage::kInterruptAcknowledge;
            g_rtc_last_hal_status = clear_status;
            return;
        }
        g_rtc_alarm_fired = true;
        ++g_rtc_wake_count;
    }
}

namespace bisen {

bool low_power_init() {
#if !BISEN_ENABLE_LOW_POWER
    return false;
#else
    if (g_rtc_initialized) {
        return true;
    }
    g_rtc_failure_stage = RtcFailureStage::kNone;
    g_rtc_last_hal_status = AM_HAL_STATUS_SUCCESS;
    NVIC_DisableIRQ(RTC_IRQn);
    NVIC_ClearPendingIRQ(RTC_IRQn);
    const am_hal_rtc_config_t rtc_config = {
        .eOscillator = AM_HAL_RTC_OSC_LFRC,
        .b12Hour = false,
    };
    am_hal_rtc_time_t time = {};
    time.ui32Year = 24u;
    time.ui32Month = 1u;
    time.ui32DayOfMonth = 1u;
    time.ui32Weekday = 1u;
    if (!rtc_hal_step(am_hal_rtc_config(&rtc_config),
                      RtcFailureStage::kConfigure) ||
        !rtc_hal_step(am_hal_rtc_osc_enable(),
                      RtcFailureStage::kOscillatorEnable) ||
        !rtc_hal_step(am_hal_rtc_time_set(&time),
                      RtcFailureStage::kTimeSet) ||
        !rtc_hal_step(am_hal_rtc_alarm_set(&time, AM_HAL_RTC_ALM_RPT_SEC),
                      RtcFailureStage::kAlarmSet) ||
        !rtc_hal_step(am_hal_rtc_interrupt_clear(AM_HAL_RTC_INT_ALM),
                      RtcFailureStage::kInterruptClear) ||
        !rtc_hal_step(am_hal_rtc_interrupt_enable(AM_HAL_RTC_INT_ALM),
                      RtcFailureStage::kInterruptEnable)) {
        return false;
    }
    NVIC_ClearPendingIRQ(RTC_IRQn);
    NVIC_EnableIRQ(RTC_IRQn);
    (void)am_hal_interrupt_master_enable();
    g_rtc_initialized = true;
    return true;
#endif
}

SleepStatus sleep_until_rtc_alarm() {
#if !BISEN_ENABLE_LOW_POWER
    return SleepStatus::kDisabled;
#else
    if (!low_power_init()) {
        return SleepStatus::kHalError;
    }
    g_rtc_alarm_fired = false;
    // neuralSPOT's Apollo4 wrapper disables UART/ITM, powers crypto down, and
    // then calls the R4.5.0 HAL with AM_HAL_SYSCTRL_SLEEP_DEEP.
    ns_deep_sleep();
    if (g_rtc_failure_stage != RtcFailureStage::kNone) {
        return SleepStatus::kHalError;
    }
    return g_rtc_alarm_fired ? SleepStatus::kWoke
                             : SleepStatus::kUnexpectedWake;
#endif
}

uint32_t rtc_wake_count() { return g_rtc_wake_count; }

RtcFailureStage rtc_failure_stage() { return g_rtc_failure_stage; }

uint32_t rtc_last_hal_status() { return g_rtc_last_hal_status; }

const char *rtc_failure_stage_name(RtcFailureStage stage) {
    switch (stage) {
        case RtcFailureStage::kNone: return "none";
        case RtcFailureStage::kConfigure: return "configure";
        case RtcFailureStage::kOscillatorEnable: return "oscillator-enable";
        case RtcFailureStage::kTimeSet: return "time-set";
        case RtcFailureStage::kAlarmSet: return "alarm-set";
        case RtcFailureStage::kInterruptClear: return "interrupt-clear";
        case RtcFailureStage::kInterruptEnable: return "interrupt-enable";
        case RtcFailureStage::kInterruptStatus: return "interrupt-status";
        case RtcFailureStage::kInterruptAcknowledge:
            return "interrupt-acknowledge";
    }
    return "unknown";
}

}  // namespace bisen
