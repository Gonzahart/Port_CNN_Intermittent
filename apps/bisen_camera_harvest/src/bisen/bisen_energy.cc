#include "bisen_energy.h"

#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "bisen_adc.h"

namespace bisen {
namespace {

#if BISEN_ENABLE_VCAP_ADC
const am_hal_adc_config_t kAdcConfig = {
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

const am_hal_adc_slot_config_t kVcapSlot = {
    .eMeasToAvg = AM_HAL_ADC_SLOT_AVG_16,
    .ui32TrkCyc = kVcapAdcTrackingCycles,
    .ePrecisionMode = AM_HAL_ADC_SLOT_12BIT,
    // AMAP4PEVB J9.8/GPIO16 is hard-wired to ADCSE3.
    .eChannel = AM_HAL_ADC_SLOT_CHSEL_SE3,
    .bWindowCompare = false,
    .bEnabled = true,
};

void adc_shutdown(void *handle) {
    if (handle != nullptr) {
        adc_interrupt_shutdown(handle);
        (void)am_hal_adc_disable(handle);
        (void)am_hal_adc_power_control(handle, AM_HAL_SYSCTRL_DEEPSLEEP, false);
        (void)am_hal_adc_deinitialize(handle);
    }
}
#endif

}  // namespace

EnergyStatus measure_energy(EnergyObservation *observation) {
    if (observation == nullptr) {
        return EnergyStatus::kInvalidArgument;
    }
    const EnergyBand previous_band = observation->previous_band;
    *observation = {};
    observation->previous_band = previous_band;

#if BISEN_TEST_VCAP_MV
    observation->valid = true;
    observation->simulated = true;
    observation->policy_vcap_millivolts = BISEN_TEST_VCAP_MV;
    observation->vcap_volts =
        static_cast<float>(observation->policy_vcap_millivolts) / 1000.0f;
    observation->nominal_vcap_volts = observation->vcap_volts;
    return EnergyStatus::kReady;
#elif !BISEN_ENABLE_VCAP_ADC
    return EnergyStatus::kHardwareUnresolved;
#else
    // This configures only the BSP's ADCSE3 function after the physical gate
    // in bisen_config.h was explicitly acknowledged.
    if (am_hal_gpio_pinconfig(kVcapAdcGpio, g_AM_BSP_GPIO_ADCSE3) !=
        AM_HAL_STATUS_SUCCESS) {
        return EnergyStatus::kGpioConfigError;
    }

    void *handle = nullptr;
    if (am_hal_adc_initialize(0u, &handle) != AM_HAL_STATUS_SUCCESS) {
        return EnergyStatus::kAdcInitializeError;
    }
    if (am_hal_adc_power_control(handle, AM_HAL_SYSCTRL_WAKE, false) !=
        AM_HAL_STATUS_SUCCESS) {
        adc_shutdown(handle);
        return EnergyStatus::kAdcPowerError;
    }
    if (am_hal_adc_configure(handle, const_cast<am_hal_adc_config_t *>(&kAdcConfig)) !=
        AM_HAL_STATUS_SUCCESS) {
        adc_shutdown(handle);
        return EnergyStatus::kAdcConfigureError;
    }
    if (am_hal_adc_configure_slot(
            handle, 0u, const_cast<am_hal_adc_slot_config_t *>(&kVcapSlot)) !=
        AM_HAL_STATUS_SUCCESS) {
        adc_shutdown(handle);
        return EnergyStatus::kAdcSlotConfigureError;
    }
    if (am_hal_adc_enable(handle) != AM_HAL_STATUS_SUCCESS) {
        adc_shutdown(handle);
        return EnergyStatus::kAdcEnableError;
    }

    // The divider's 10 nF capacitor needs multiple RC time constants. This
    // delay is intentionally longer than the ADC's own LPMODE1 reference wake.
    am_hal_delay_us(kVcapAdcSettleUs);
    // Discard the first conversion after wake so reference and sample/hold
    // startup cannot be mistaken for the divider voltage.
    am_hal_adc_sample_t discarded_sample = {};
    const bool primed = adc_trigger_and_read_interrupt(
                            handle, 0u, &discarded_sample) ==
                        AdcSampleStatus::kReady;
    am_hal_adc_sample_t sample = {};
    const bool read_ok =
        primed && adc_trigger_and_read_interrupt(handle, 0u, &sample) ==
                      AdcSampleStatus::kReady;
    adc_shutdown(handle);
    if (!read_ok) {
        return EnergyStatus::kAdcSampleError;
    }

    observation->priming_adc_code = discarded_sample.ui32Sample;
    observation->corrected_adc_code = sample.ui32Sample;
    observation->adc_volts =
        (static_cast<float>(sample.ui32Sample) / AM_HAL_ADC_SAMPLE_DIVISORF) *
        kAdcReferenceVolts;
    observation->nominal_vcap_volts =
        observation->adc_volts * kVcapDividerScale;
    observation->policy_vcap_millivolts =
        calibrated_vcap_millivolts_from_code(sample.ui32Sample);
    observation->vcap_volts =
        static_cast<float>(observation->policy_vcap_millivolts) / 1000.0f;
    if (observation->adc_volts > kVcapAdcMaxValidationVolts ||
        observation->nominal_vcap_volts > kVcapMaxValidationVolts) {
        return EnergyStatus::kAdcOutOfValidationRange;
    }
    observation->valid = true;
    return EnergyStatus::kReady;
#endif
}

}  // namespace bisen
