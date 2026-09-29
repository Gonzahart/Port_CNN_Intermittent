#include "bisen_adc.h"

namespace {

constexpr uint32_t kAdcCompletionInterrupt = AM_HAL_ADC_INT_SCNCMP;
#if BISEN_ENABLE_ADC_DIAGNOSTIC
constexpr uint32_t kAdcObservationInterrupt = AM_HAL_ADC_INT_CNVCMP;
#else
constexpr uint32_t kAdcObservationInterrupt = 0u;
#endif
constexpr uint32_t kAdcErrorInterrupts =
    AM_HAL_ADC_INT_DERR | AM_HAL_ADC_INT_FIFOOVR1 |
    AM_HAL_ADC_INT_FIFOOVR2;
constexpr uint32_t kAdcInterrupts =
    kAdcCompletionInterrupt | kAdcObservationInterrupt | kAdcErrorInterrupts;

#if BISEN_ENABLE_ADC_DIAGNOSTIC
// The installed neuralSPOT Apollo4 temperature reference repeats a software
// trigger with a 1 us interval until FIFO COUNT changes. Keep that SDK
// sequence bounded in the diagnostic so a failed ADC cannot hang the target.
constexpr uint32_t kAdcTriggerAttemptLimit = 32u;
#else
// Both production slots use AM_HAL_ADC_SLOT_AVG_16. Hardware validation in
// LPMODE0 and LPMODE1 showed that scans 1..15 leave the FIFO empty and scan 16
// emits the averaged result. Bound production to that configured, exact count
// rather than using the installed reference's unbounded FIFO loop.
constexpr uint32_t kAdcTriggerAttemptLimit = 16u;
#endif

void *volatile g_adc_handle = nullptr;
volatile uint32_t g_adc_interrupt_status = 0u;
volatile bool g_adc_interrupt_complete = false;
volatile bool g_adc_interrupt_hal_error = false;

#if BISEN_ENABLE_ADC_DIAGNOSTIC
bisen::AdcDebugSnapshot g_adc_debug_snapshot = {};

void capture_before_trigger(uint32_t trigger_attempt) {
    g_adc_debug_snapshot.cfg_before_trigger = ADC->CFG;
    g_adc_debug_snapshot.stat_before_trigger = ADC->STAT;
    g_adc_debug_snapshot.slot0_before_trigger = ADC->SL0CFG;
    g_adc_debug_snapshot.fifo_before_trigger = ADC->FIFO;
    g_adc_debug_snapshot.inten_before_trigger = ADC->INTEN;
    g_adc_debug_snapshot.intstat_before_trigger = ADC->INTSTAT;
    g_adc_debug_snapshot.trigger_attempts = trigger_attempt;
}

void capture_after_interrupt(uint32_t interrupt_status) {
    g_adc_debug_snapshot.cfg_after_interrupt = ADC->CFG;
    g_adc_debug_snapshot.stat_after_interrupt = ADC->STAT;
    g_adc_debug_snapshot.slot0_after_interrupt = ADC->SL0CFG;
    g_adc_debug_snapshot.fifo_after_interrupt = ADC->FIFO;
    g_adc_debug_snapshot.inten_after_interrupt = ADC->INTEN;
    g_adc_debug_snapshot.intstat_after_interrupt = ADC->INTSTAT;
    g_adc_debug_snapshot.interrupt_status = interrupt_status;
    g_adc_debug_snapshot.accumulated_interrupt_status |= interrupt_status;
    g_adc_debug_snapshot.devpwren = PWRCTRL->DEVPWREN;
    g_adc_debug_snapshot.devpwrstatus = PWRCTRL->DEVPWRSTATUS;
    g_adc_debug_snapshot.adcpwrctrl = MCUCTRL->ADCPWRCTRL;
    g_adc_debug_snapshot.adcpwrdly = MCUCTRL->ADCPWRDLY;
}
#endif

void clear_interrupt_state(void *handle) {
    NVIC_DisableIRQ(ADC_IRQn);
    NVIC_ClearPendingIRQ(ADC_IRQn);
    (void)am_hal_adc_interrupt_disable(handle, kAdcInterrupts);
    (void)am_hal_adc_interrupt_clear(handle, kAdcInterrupts);
    g_adc_handle = nullptr;
    g_adc_interrupt_status = 0u;
    g_adc_interrupt_complete = false;
    g_adc_interrupt_hal_error = false;
}

}  // namespace

extern "C" void am_adc_isr(void) {
    void *handle = g_adc_handle;
    if (handle == nullptr) {
        NVIC_DisableIRQ(ADC_IRQn);
        return;
    }

    uint32_t status = 0u;
    if (am_hal_adc_interrupt_status(handle, &status, true) !=
        AM_HAL_STATUS_SUCCESS) {
        g_adc_interrupt_hal_error = true;
        g_adc_interrupt_complete = true;
        return;
    }
    if (status != 0u &&
        am_hal_adc_interrupt_clear(handle, status) != AM_HAL_STATUS_SUCCESS) {
        g_adc_interrupt_hal_error = true;
        g_adc_interrupt_complete = true;
        return;
    }

    g_adc_interrupt_status |= status;
    if ((status & (kAdcCompletionInterrupt | kAdcErrorInterrupts)) != 0u) {
        g_adc_interrupt_complete = true;
    }
}

namespace bisen {

AdcSampleStatus adc_trigger_and_read_interrupt(
    void *handle, uint32_t expected_slot, am_hal_adc_sample_t *sample) {
    if (handle == nullptr || sample == nullptr) {
        return AdcSampleStatus::kInvalidArgument;
    }
    if (g_adc_handle != nullptr) {
        return AdcSampleStatus::kBusy;
    }

    g_adc_interrupt_status = 0u;
    g_adc_interrupt_complete = false;
    g_adc_interrupt_hal_error = false;
    g_adc_handle = handle;

#if BISEN_ENABLE_ADC_DIAGNOSTIC
    g_adc_debug_snapshot = {};
#endif

    NVIC_DisableIRQ(ADC_IRQn);
    NVIC_ClearPendingIRQ(ADC_IRQn);
    if (am_hal_adc_interrupt_clear(handle, kAdcInterrupts) !=
            AM_HAL_STATUS_SUCCESS ||
        am_hal_adc_interrupt_enable(handle, kAdcInterrupts) !=
            AM_HAL_STATUS_SUCCESS) {
        clear_interrupt_state(handle);
        return AdcSampleStatus::kInterruptConfigureError;
    }
    NVIC_SetPriority(ADC_IRQn, AM_IRQ_PRIORITY_DEFAULT);
    NVIC_EnableIRQ(ADC_IRQn);

    // Preserve the caller's global interrupt state while allowing this
    // synchronous operation to receive its completion IRQ.
    const uint32_t previous_master_state = am_hal_interrupt_master_enable();
    uint32_t fifo_state = 0u;
    uint32_t interrupt_status = 0u;
    bool hal_error = false;
    for (uint32_t attempt = 1u; attempt <= kAdcTriggerAttemptLimit; ++attempt) {
        g_adc_interrupt_status = 0u;
        g_adc_interrupt_complete = false;
        g_adc_interrupt_hal_error = false;
        NVIC_ClearPendingIRQ(ADC_IRQn);
        if (am_hal_adc_interrupt_clear(handle, kAdcInterrupts) !=
            AM_HAL_STATUS_SUCCESS) {
            hal_error = true;
            break;
        }
#if BISEN_ENABLE_ADC_DIAGNOSTIC
        capture_before_trigger(attempt);
#endif
        if (am_hal_adc_sw_trigger(handle) != AM_HAL_STATUS_SUCCESS) {
            am_hal_interrupt_master_set(previous_master_state);
            clear_interrupt_state(handle);
            return AdcSampleStatus::kTriggerError;
        }

        while (!g_adc_interrupt_complete) {
            // Closing the condition/WFI race under PRIMASK prevents a
            // completion that arrives between the test and WFI from being
            // missed. CNVCMP may wake the core first; SCNCMP or an error ends
            // this scan attempt.
            const uint32_t interrupt_state = am_hal_interrupt_master_disable();
            if (!g_adc_interrupt_complete) {
                SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
                __DSB();
                __WFI();
            }
            am_hal_interrupt_master_set(interrupt_state);
        }

        interrupt_status = g_adc_interrupt_status;
        hal_error = g_adc_interrupt_hal_error;
#if BISEN_ENABLE_ADC_DIAGNOSTIC
        capture_after_interrupt(interrupt_status);
#endif
        // FIFO is non-destructive on read and its COUNT field is the hardware
        // source of truth. R4.5.0 am_hal_adc_samples_read() reads FIFOPR
        // directly; an empty FIFO can otherwise look like a successful
        // slot-0/code-0 sample.
        fifo_state = ADC->FIFO;
        if (hal_error ||
            (interrupt_status & kAdcCompletionInterrupt) == 0u ||
            (interrupt_status & kAdcErrorInterrupts) != 0u ||
            AM_HAL_ADC_FIFO_COUNT(fifo_state) != 0u) {
            break;
        }
#if BISEN_ENABLE_ADC_DIAGNOSTIC
        ++g_adc_debug_snapshot.empty_scan_count;
#endif
        // Match the installed neuralSPOT Apollo4 adc_trigger_wait() interval.
        am_hal_delay_us(1u);
    }
    am_hal_interrupt_master_set(previous_master_state);

    clear_interrupt_state(handle);
    if (hal_error || (interrupt_status & kAdcCompletionInterrupt) == 0u ||
        (interrupt_status & kAdcErrorInterrupts) != 0u) {
        return AdcSampleStatus::kPeripheralError;
    }
    if (AM_HAL_ADC_FIFO_COUNT(fifo_state) == 0u) {
        return AdcSampleStatus::kFifoEmpty;
    }

    // Follow the installed Apollo4 ADC path before loading the new FIFO value
    // through the CPU/DAXI path.
    (void)am_hal_daxi_control(AM_HAL_DAXI_CONTROL_INVALIDATE, nullptr);
    uint32_t sample_count = 1u;
    if (am_hal_adc_samples_read(handle, false, nullptr, &sample_count, sample) !=
            AM_HAL_STATUS_SUCCESS ||
        sample_count != 1u) {
        return AdcSampleStatus::kReadError;
    }
    return sample->ui32Slot == expected_slot
               ? AdcSampleStatus::kReady
               : AdcSampleStatus::kUnexpectedSlot;
}

void adc_interrupt_shutdown(void *handle) {
    if (handle != nullptr) {
        clear_interrupt_state(handle);
    }
}

#if BISEN_ENABLE_ADC_DIAGNOSTIC
AdcDebugSnapshot adc_debug_snapshot() {
    return g_adc_debug_snapshot;
}
#endif

}  // namespace bisen
