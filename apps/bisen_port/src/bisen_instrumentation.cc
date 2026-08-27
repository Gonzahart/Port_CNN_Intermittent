#include "bisen_instrumentation.h"

#if BISEN_ENABLE_GPIO_INSTRUMENTATION

#include "am_mcu_apollo.h"

namespace bisen {
namespace {

constexpr uint32_t kStatePins[] = {
    kInstrumentationState0Gpio,
    kInstrumentationState1Gpio,
    kInstrumentationState2Gpio,
};

bool g_instrumentation_ready = false;

am_hal_gpio_pincfg_t state_output_config() {
    // AM_HAL_GPIO_PINCFG_OUTPUT uses nested C designated initializers that
    // are not valid in this C++ translation unit. Assign the same documented
    // fields explicitly and leave every reserved/unused field zero.
    am_hal_gpio_pincfg_t config = {};
    config.GP.cfg_b.uFuncSel = 3u;
    config.GP.cfg_b.eGPOutCfg = AM_HAL_GPIO_PIN_OUTCFG_PUSHPULL;
    config.GP.cfg_b.eDriveStrength = AM_HAL_GPIO_PIN_DRIVESTRENGTH_0P1X;
    config.GP.cfg_b.ePullup = AM_HAL_GPIO_PIN_PULLUP_NONE;
    config.GP.cfg_b.eGPInput = AM_HAL_GPIO_PIN_INPUT_NONE;
    config.GP.cfg_b.eGPRdZero = AM_HAL_GPIO_PIN_RDZERO_READPIN;
    config.GP.cfg_b.eIntDir = AM_HAL_GPIO_PIN_INTDIR_LO2HI;
    return config;
}

InstrumentationCode state_code(State state) {
    switch (state) {
        case State::kWakeRestore:
            return InstrumentationCode::kBootOrError;
        case State::kAdc:
            return InstrumentationCode::kAdc;
        case State::kTemperatureSense:
            return InstrumentationCode::kTemperature;
        case State::kCompute:
            return InstrumentationCode::kCompute;
        case State::kNonvolatileCheckpoint:
            return InstrumentationCode::kNonvolatileWrite;
        case State::kSleepLowPowerWait:
            return InstrumentationCode::kSleep;
    }
    return InstrumentationCode::kBootOrError;
}

}  // namespace

InstrumentationStatus instrumentation_init() {
    const am_hal_gpio_pincfg_t output_config = state_output_config();
    // Clear each output latch before switching the pad to GPIO output so
    // initialization cannot create an unintended high pulse.
    for (const uint32_t pin : kStatePins) {
        am_hal_gpio_output_clear(pin);
        if (am_hal_gpio_pinconfig(pin, output_config) !=
            AM_HAL_STATUS_SUCCESS) {
            return InstrumentationStatus::kGpioConfigError;
        }
    }

    g_instrumentation_ready = true;
    instrumentation_set_code(InstrumentationCode::kBootOrError);
    return InstrumentationStatus::kReady;
}

void instrumentation_set_code(InstrumentationCode code) {
    if (!g_instrumentation_ready) {
        return;
    }

    const uint8_t value = static_cast<uint8_t>(code) & 0x07u;

    // The pins share one GPIO register bank. Clear the three-bit bus before
    // setting the new value; decoders should sample stable code intervals and
    // ignore the sub-instruction transition through zero.
    for (const uint32_t pin : kStatePins) {
        am_hal_gpio_output_clear(pin);
    }
    for (uint32_t bit = 0u; bit < 3u; ++bit) {
        if ((value & (1u << bit)) != 0u) {
            am_hal_gpio_output_set(kStatePins[bit]);
        }
    }
}

void instrumentation_set_state(State state) {
    instrumentation_set_code(state_code(state));
}

}  // namespace bisen

#endif  // BISEN_ENABLE_GPIO_INSTRUMENTATION
