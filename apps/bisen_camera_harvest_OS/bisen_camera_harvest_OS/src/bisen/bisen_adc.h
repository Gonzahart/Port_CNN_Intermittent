#ifndef BISEN_ADC_H
#define BISEN_ADC_H

#include <stdint.h>

#include "am_mcu_apollo.h"
#include "bisen_config.h"

namespace bisen {

enum class AdcSampleStatus : uint8_t {
    kReady = 0,
    kInvalidArgument,
    kBusy,
    kInterruptConfigureError,
    kTriggerError,
    kPeripheralError,
    kReadError,
    kUnexpectedSlot,
    kFifoEmpty,
};

#if BISEN_ENABLE_ADC_DIAGNOSTIC
struct AdcDebugSnapshot {
    uint32_t cfg_before_trigger;
    uint32_t stat_before_trigger;
    uint32_t slot0_before_trigger;
    uint32_t fifo_before_trigger;
    uint32_t inten_before_trigger;
    uint32_t intstat_before_trigger;
    uint32_t cfg_after_interrupt;
    uint32_t stat_after_interrupt;
    uint32_t slot0_after_interrupt;
    uint32_t fifo_after_interrupt;
    uint32_t inten_after_interrupt;
    uint32_t intstat_after_interrupt;
    uint32_t interrupt_status;
    uint32_t accumulated_interrupt_status;
    uint32_t trigger_attempts;
    uint32_t empty_scan_count;
    uint32_t devpwren;
    uint32_t devpwrstatus;
    uint32_t adcpwrctrl;
    uint32_t adcpwrdly;
};

// Return the register state captured around the most recent software-triggered
// conversion. This exists only in the forced-MRAM-off ADC diagnostic build.
AdcDebugSnapshot adc_debug_snapshot();
#endif

// Supply bounded software triggers until the configured hardware averaging
// emits one FIFO sample. Each trigger waits in normal sleep for the ADC
// scan-complete interrupt. The ISR only records status; sample handling remains
// in thread context, matching the MSP430 BISen execution model.
AdcSampleStatus adc_trigger_and_read_interrupt(
    void *handle, uint32_t expected_slot, am_hal_adc_sample_t *sample);

// Disable the shared ADC interrupt path before the caller powers down or
// deinitializes ADC instance zero.
void adc_interrupt_shutdown(void *handle);

}  // namespace bisen

#endif  // BISEN_ADC_H
