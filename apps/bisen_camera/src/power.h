// power.h -- per-phase energy estimate for one frame.
//
// Answers "where does the energy go", which is the question the project is
// actually being judged on. It is an ESTIMATE: phase durations are measured on
// the board with STIMER, but the currents come from the Apollo4 Plus datasheet,
// not from a meter. Swap the constants in power.c for measured values and the
// numbers become real without any other change.
//
// What it does NOT model: the analog front end (photodiode array, ADG732 mux,
// the RC network), the ADC's own supply current, the status LEDs, and the
// J-Link/VCOM. So this is core energy, not board energy -- do not quote it as
// total system power.
#ifndef POWER_H
#define POWER_H

#include <stdint.h>

// What the core does while a photodiode settles. 3.5 ms per pixel, ~1000
// pixels a frame, so this one choice is most of the frame's energy.
//
//   0  spin   am_util_delay_us  -- ~2045 uA. The original behaviour.
//   1  sleep  WFI, clocks gated, oscillators still running -- ~180 uA.
//   2  deep   WFI with SLEEPDEEP -- ~26 uA at best, but see the caveat below.
//
// 1 is the default because it cannot lose its wake-up: the oscillators keep
// running, so STIMER (which times the wait AND the energy report) survives.
// Under 2 the datasheet allows HFRC to be gated in the deeper states, and if
// that happened the wake source would stop -- so it is opt-in.
//
// Either way the wait ends by re-checking STIMER and spinning out whatever
// remains, so a sleep that never wakes degrades to mode 0 rather than hanging
// the scan or short-changing a pixel's settling time.
// ⚠️ 2026-08-17: mode 1 HUNG THE BOARD on the first attempt -- no console,
// buttons dead. The cause was `AM_HAL_STIMER_CFG_COMPARE_A_ENABLE` missing
// from the STIMER config, so the compare could never fire and WFI never
// returned. A spin-out fallback placed AFTER the sleep was useless, because
// control never got there.
//
// Now: pwr_wait_init() enables compare A and then PROVES the interrupt fires
// by POLLING for it, before any WFI is ever executed. Sleeping is permitted
// only if that test passed; otherwise every wait spins. Self-test observed
// passing on hardware 2026-08-17, so mode 1 is the default.
// Mode 2 (deep sleep) is being TRIED 2026-08-17. It is the one variant the
// boot self-test cannot vouch for: that test polls while awake, whereas mode
// 2's risk -- HFRC being gated, taking STIMER and therefore the wake source
// with it -- only exists inside the sleep. If the board goes dead, rebuild
// with 1 and reflash; the last console line will be the deep-sleep marker.
#ifndef SCAN_IDLE_MODE
#define SCAN_IDLE_MODE 2
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The settling wait, honouring SCAN_IDLE_MODE. Lives with the energy model
// because the mode chosen here is what the model has to charge for.
void pwr_wait_us(uint32_t us);

// One-time setup of the wake source. Call before the first pwr_wait_us.
void pwr_wait_init(void);

// What the core is doing. Every microsecond of a frame lands in exactly one.
typedef enum {
    PWR_OTHER = 0,   // anything not attributed below
    PWR_SETTLE,      // waiting for the photodiode node to settle
    PWR_ADC,         // ADC: triggering, polling, reading the FIFO
    PWR_ADCWAIT,     // ADC: the part of the conversion we sleep through
    PWR_INFER,       // the engine
    PWR_UART,        // shipping the frame and the result out
    PWR_IDLE,        // between-frame delay
    PWR_COUNT
} pwr_phase_t;

// Start attributing a new frame. Call once per frame, before the scan.
void pwr_frame_start(void);

// Attribute the following code to `p`. Returns the phase that was running, to
// be handed back to pwr_end -- so these nest without a stack.
pwr_phase_t pwr_begin(pwr_phase_t p);
void pwr_end(pwr_phase_t prev);

// Print the per-phase time and energy breakdown for the frame just finished.
void pwr_report(void);

// Frame energy in nanojoules, for a caller that wants the raw number.
uint64_t pwr_frame_nj(void);

#ifdef __cplusplus
}
#endif
#endif  // POWER_H
