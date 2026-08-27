#include "bisen_temperature.h"

#include "am_mcu_apollo.h"
#include "bisen_adc.h"

namespace bisen {
namespace {

const am_hal_adc_config_t kTemperatureAdcConfig = {
    .eClock = AM_HAL_ADC_CLKSEL_HFRC_24MHZ,
    .eRepeatTrigger = AM_HAL_ADC_RPTTRIGSEL_TMR,
    .ePolarity = AM_HAL_ADC_TRIGPOL_RISING,
    .eTrigger = AM_HAL_ADC_TRIGSEL_SOFTWARE,
    .eClockMode = AM_HAL_ADC_CLKMODE_LOW_POWER,
    // Match the installed neuralSPOT Apollo4 temperature reference. The ADC
    // helper supplies the repeated software triggers required by AVG_16.
    .ePowerMode = AM_HAL_ADC_LPMODE1,
    .eRepeat = AM_HAL_ADC_SINGLE_SCAN,
};

const am_hal_adc_slot_config_t kTemperatureSlot = {
    .eMeasToAvg = AM_HAL_ADC_SLOT_AVG_16,
    .ui32TrkCyc = AM_HAL_ADC_MIN_TRKCYC,
    .ePrecisionMode = AM_HAL_ADC_SLOT_12BIT,
    .eChannel = AM_HAL_ADC_SLOT_CHSEL_TEMP,
    .bWindowCompare = false,
    .bEnabled = true,
};

void shutdown_adc(void *handle) {
    if (handle != nullptr) {
        adc_interrupt_shutdown(handle);
        (void)am_hal_adc_disable(handle);
        (void)am_hal_adc_power_control(handle, AM_HAL_SYSCTRL_DEEPSLEEP, false);
        (void)am_hal_adc_deinitialize(handle);
    }
}

}  // namespace

TemperatureStatus measure_die_temperature(TemperatureObservation *observation) {
    if (observation == nullptr) {
        return TemperatureStatus::kInvalidArgument;
    }
    *observation = {};
    void *handle = nullptr;
    if (am_hal_adc_initialize(0u, &handle) != AM_HAL_STATUS_SUCCESS) {
        return TemperatureStatus::kAdcInitializeError;
    }
    if (am_hal_adc_power_control(handle, AM_HAL_SYSCTRL_WAKE, false) !=
        AM_HAL_STATUS_SUCCESS) {
        shutdown_adc(handle);
        return TemperatureStatus::kAdcPowerError;
    }
    if (am_hal_adc_configure(
            handle, const_cast<am_hal_adc_config_t *>(&kTemperatureAdcConfig)) !=
        AM_HAL_STATUS_SUCCESS) {
        shutdown_adc(handle);
        return TemperatureStatus::kAdcConfigureError;
    }
    if (am_hal_adc_configure_slot(
            handle, 0u, const_cast<am_hal_adc_slot_config_t *>(&kTemperatureSlot)) !=
        AM_HAL_STATUS_SUCCESS) {
        shutdown_adc(handle);
        return TemperatureStatus::kAdcSlotConfigureError;
    }
    if (am_hal_adc_enable(handle) != AM_HAL_STATUS_SUCCESS) {
        shutdown_adc(handle);
        return TemperatureStatus::kAdcEnableError;
    }

    // Discard the first conversion after wake, matching the validated VCAP
    // sequence, so reference/sensor startup cannot become the recorded value.
    am_hal_adc_sample_t discarded_sample = {};
    if (adc_trigger_and_read_interrupt(handle, 0u, &discarded_sample) !=
        AdcSampleStatus::kReady) {
        shutdown_adc(handle);
        return TemperatureStatus::kAdcPrimeError;
    }
    am_hal_adc_sample_t sample = {};
    if (adc_trigger_and_read_interrupt(handle, 0u, &sample) !=
        AdcSampleStatus::kReady) {
        shutdown_adc(handle);
        return TemperatureStatus::kAdcSampleError;
    }

    float temperature_args[3] = {
        (static_cast<float>(sample.ui32Sample) / AM_HAL_ADC_SAMPLE_DIVISORF) *
            AM_HAL_ADC_VREF,
        0.0f,
        -123.456f,
    };
    const uint32_t status = am_hal_adc_control(
        handle, AM_HAL_ADC_REQ_TEMP_CELSIUS_GET, temperature_args);
    shutdown_adc(handle);
    if (status != AM_HAL_STATUS_SUCCESS) {
        return TemperatureStatus::kCelsiusConversionError;
    }
    observation->valid = true;
    observation->corrected_adc_code = sample.ui32Sample;
    observation->sensor_volts = temperature_args[0];
    observation->celsius = temperature_args[1];
    return TemperatureStatus::kReady;
}

}  // namespace bisen
