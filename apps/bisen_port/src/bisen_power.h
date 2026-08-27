#ifndef BISEN_POWER_H
#define BISEN_POWER_H

#include <stdbool.h>
#include <stdint.h>

namespace bisen {

enum class SleepStatus : uint8_t {
    kWoke = 0,
    kDisabled,
    kHalError,
    kUnexpectedWake,
};

enum class RtcFailureStage : uint8_t {
    kNone = 0,
    kConfigure,
    kOscillatorEnable,
    kTimeSet,
    kAlarmSet,
    kInterruptClear,
    kInterruptEnable,
    kInterruptStatus,
    kInterruptAcknowledge,
};

bool low_power_init();
SleepStatus sleep_until_rtc_alarm();
uint32_t rtc_wake_count();
RtcFailureStage rtc_failure_stage();
uint32_t rtc_last_hal_status();
const char *rtc_failure_stage_name(RtcFailureStage stage);

}  // namespace bisen

#endif  // BISEN_POWER_H
