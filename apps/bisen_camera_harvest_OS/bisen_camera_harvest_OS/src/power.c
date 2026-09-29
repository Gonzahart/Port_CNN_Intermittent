
// power.c -- per-phase energy estimate. See power.h.
//
// Time is measured with STIMER, which runs from HFRC at a fixed 6 MHz and is
// NOT derived from the core clock. That matters here: the whole point of this
// module is to evaluate changing the core clock, and a core-clock-derived timer
// would move underneath the measurement.
#include "power.h"

#include "am_mcu_apollo.h"
#include "am_util.h"
#include "log_control.h"

#define STIMER_HZ 6000000u

// ---------------------------------------------------------------------------
// Current model. Apollo4 Plus SoC Datasheet, section 29.4, Table 29 "Current
// Consumption in Active Mode and Sleep Modes", VDD = 1.9 V, Typ without PRO.
//
// Note the datasheet's headline "4 uA/MHz" (p.2) is not any of these -- the
// measured Coremark figures are 2-5x higher. Use the table, not the front page.
// ---------------------------------------------------------------------------
#define PWR_VDD_MV 1900u

// IRUNLPFB  17.2 uA/MHz @ 96 MHz  -- working code, low power mode
// IRUNHPFB  21.3 uA/MHz @ 192 MHz -- working code, high performance mode
#define UA_ACTIVE_HP 4090u   /* 21.3 * 192 */
#define UA_ACTIVE_LP 1651u   /* 17.2 * 96  */

// IRUNWLPFB 8.6 uA/MHz -- a *while loop*, which is exactly what
// am_util_delay_us() is. Much cheaper than Coremark because it misses cache
// rarely and does no memory traffic.
//
// The table gives this for LP only. The HP figure below is DERIVED by scaling
// with the Coremark HP/LP ratio (21.3/17.2 = 1.24), not measured. Flagged
// because it is the single largest term in the current firmware's frame.
#define UA_SPIN_LP 826u    /* 8.6 * 96 */
#define UA_SPIN_HP 2045u   /* 8.6 * 1.24 * 192 -- DERIVED, see above */

#ifndef BISEN_HARVEST_MCU_LOW_POWER
#define BISEN_HARVEST_MCU_LOW_POWER 0
#endif
#if BISEN_HARVEST_MCU_LOW_POWER
#define UA_ACTIVE_MODE UA_ACTIVE_LP
#define UA_SPIN_MODE UA_SPIN_LP
#else
#define UA_ACTIVE_MODE UA_ACTIVE_HP
#define UA_SPIN_MODE UA_SPIN_HP
#endif

// ISDS2-384RET 25.6 uA -- deep sleep 2, 384 kB TCM retained, buck on, XTAL off.
// 384 kB is chosen because the frame buffer and the context have to survive.
//
// ⚠️ UNVERIFIED FOR OUR CASE. That figure is for SDS2, where HFRC is off. We
// wake from STIMER, which runs on HFRC, so STIMER holds HFRC up and we most
// likely land in SDS0/SDS1 instead -- where the datasheet says "HFRC is on" and
// the current is higher, somewhere between this and UA_SLEEP. Treat any mode-2
// energy figure as a lower bound until a meter says otherwise.
#define UA_DEEPSLEEP 26u

// ISS2 180 uA -- ordinary sleep, clocks gated, oscillators still running.
#define UA_SLEEP 180u

// The ADC's own supply current is NOT published in the datasheet's ADC table
// (Table 41 lists clock and performance only). Left at zero deliberately rather
// than guessed, so the report can say what it does not cover.
#define UA_ADC_ADDER 0u

// Current attributed to each phase, given how the firmware is written TODAY.
// Changing the implementation means changing these -- that is the point: the
// report then shows what the change bought.
// What the settling wait actually costs is whatever SCAN_IDLE_MODE picked.
// Derived from the mode rather than written out separately, so the report can
// never claim a saving the firmware did not take.
#if SCAN_IDLE_MODE == 2
#define UA_SETTLE UA_DEEPSLEEP
#elif SCAN_IDLE_MODE == 1
#define UA_SETTLE UA_SLEEP
#else
#define UA_SETTLE UA_SPIN_MODE
#endif

static const uint32_t s_ua[PWR_COUNT] = {
    [PWR_OTHER]   = UA_ACTIVE_MODE,
    [PWR_SETTLE]  = UA_SETTLE,
    [PWR_ADC]     = UA_ACTIVE_MODE + UA_ADC_ADDER,  // trigger, poll, FIFO read
    [PWR_ADCWAIT] = UA_SETTLE,                    // slept, so priced as sleep
    [PWR_INFER]   = UA_ACTIVE_MODE,
    [PWR_UART]    = UA_SPIN_MODE,                 // blocks on the UART FIFO
    [PWR_IDLE]    = UA_SETTLE,                    // same wait as the settling
};

#if BISEN_ENABLE_SWO_LOGGING
static const char *const s_name[PWR_COUNT] = {
    "other", "settle", "adc", "adcwait", "infer", "uart", "idle",
};
#endif

static uint32_t s_tk[PWR_COUNT];
static pwr_phase_t s_cur;
static uint32_t s_last;

// Close the books on whatever phase is running and open them on the next.
// Unsigned arithmetic makes the STIMER's 32-bit wrap (~716 s at 6 MHz) work
// out correctly, and a frame is a few seconds.
static void flush(void) {
    uint32_t now = am_hal_stimer_counter_get();
    s_tk[s_cur] += now - s_last;
    s_last = now;
}

void pwr_frame_start(void) {
    for (int i = 0; i < PWR_COUNT; i++) s_tk[i] = 0;
    s_cur = PWR_OTHER;
    s_last = am_hal_stimer_counter_get();
}

pwr_phase_t pwr_begin(pwr_phase_t p) {
    flush();
    pwr_phase_t prev = s_cur;
    s_cur = p;
    return prev;
}

void pwr_end(pwr_phase_t prev) {
    flush();
    s_cur = prev;
}

static uint32_t ticks_to_us(uint32_t tk) {
    return (uint32_t)((uint64_t)tk * 1000000u / STIMER_HZ);
}

// uA * mV * us = femtojoules; /1e6 gives nanojoules.
static uint64_t phase_nj(int i) {
    uint64_t us = (uint64_t)ticks_to_us(s_tk[i]);
    return (uint64_t)s_ua[i] * PWR_VDD_MV * us / 1000000u;
}

uint64_t pwr_frame_nj(void) {
    uint64_t t = 0;
    for (int i = 0; i < PWR_COUNT; i++) t += phase_nj(i);
    return t;
}

// ---------------------------------------------------------------------------
// The settling wait.
//
// STIMER compare 0 is the wake source. It is a good fit: the counter is
// already running for the timing above, it is not in the debug power domain,
// and nothing else in this app uses a compare register.
//
// The default handler for this vector is a weak symbol, so defining it here
// takes ownership without touching the SDK. All it has to do is clear the
// interrupt -- waking up IS the work.
// ---------------------------------------------------------------------------
#if SCAN_IDLE_MODE != 0
void am_stimer_cmpr0_isr(void) {
    am_hal_stimer_int_clear(AM_HAL_STIMER_INT_COMPAREA);
}
#endif

// Set only once the wake source has been OBSERVED to work. Until then every
// wait spins, so a board that cannot wake is slow rather than dead.
static uint8_t s_sleep_ok = 0;

void pwr_wait_init(void) {
    // 1. Compare A must be enabled in the STIMER config or the compare never
    //    fires -- cam_lenet.cc sets the clock source only. This is exactly why
    //    the first attempt hung: the interrupt was impossible, so WFI never
    //    returned. No CFG_CLEAR here, the counter must keep running for the
    //    timing everything else depends on.
    am_hal_stimer_config(AM_HAL_STIMER_HFRC_6MHZ |
                         AM_HAL_STIMER_CFG_COMPARE_A_ENABLE);

    // 2. Arm the compare at the STIMER, but leave the NVIC alone for now: the
    //    ISR would clear the status flag before the poll below could see it.
    am_hal_stimer_int_clear(AM_HAL_STIMER_INT_COMPAREA);
    am_hal_stimer_int_enable(AM_HAL_STIMER_INT_COMPAREA);

    // 3. PROVE it fires -- by POLLING, never by sleeping. A compare that does
    //    not fire costs a 20 ms spin here instead of a hung board. This runs
    //    even when sleeping is disabled, so a safe build still reports whether
    //    sleep WOULD work.
    const uint32_t t0 = am_hal_stimer_counter_get();
    am_hal_stimer_compare_delta_set(0, 6000);            // 1 ms
    while ((am_hal_stimer_counter_get() - t0) < 120000u) {   // 20 ms budget
        if (am_hal_stimer_int_status_get(false) & AM_HAL_STIMER_INT_COMPAREA) {
            s_sleep_ok = 1;
            break;
        }
    }
    am_hal_stimer_int_clear(AM_HAL_STIMER_INT_COMPAREA);

    // 4. Only now hand the vector to the NVIC, and only if it works.
    if (s_sleep_ok) {
        NVIC_ClearPendingIRQ(STIMER_CMPR0_IRQn);
        NVIC_EnableIRQ(STIMER_CMPR0_IRQn);
    }

    am_util_stdio_printf(
        "POWER: STIMER compare self-test %s (sleep mode %d -> %s)\n",
        s_sleep_ok ? "PASSED" : "FAILED",
        SCAN_IDLE_MODE,
        (s_sleep_ok && SCAN_IDLE_MODE) ? "sleeping" : "spin-wait");
}

// Did the sleep actually happen? The energy report cannot tell you -- it
// charges the sleep current because that is what the build asked for, not
// because it observed it. These counters can: if the wake source never fires,
// every wait falls through to the spin fallback and `spun` tracks `waits`.
// Reported per frame, so a broken sleep is visible on the console rather than
// quietly costing 11x the energy the report claims.
uint32_t g_pwr_waits = 0;
uint32_t g_pwr_spun = 0;

void pwr_wait_us(uint32_t us) {
#if SCAN_IDLE_MODE == 0
    am_util_delay_us(us);
#else
    // The gate that makes hanging impossible: no WFI is ever executed unless
    // the self-test watched the compare fire.
    if (!s_sleep_ok) {
        am_util_delay_us(us);
        return;
    }

    const uint32_t ticks = us * (STIMER_HZ / 1000000u);
    const uint32_t t0 = am_hal_stimer_counter_get();

    g_pwr_waits++;
    am_hal_stimer_compare_delta_set(0, ticks);

#if SCAN_IDLE_MODE == 2
    // Deep sleep can gate HFRC, and STIMER -- the wake source -- runs from it.
    // If that happens WFI never returns and the board is dead with no output.
    // Announcing the first entry means the console's last line says exactly
    // where it stopped, instead of leaving us to guess.
    static uint8_t s_announced;
    if (!s_announced) {
        s_announced = 1;
        am_util_stdio_printf("POWER: entering DEEP sleep (mode 2) for the "
                             "first time -- if output stops here, HFRC was "
                             "gated and the wake source died\n");
    }
#endif

    // Sleep until the compare fires, re-checking the counter each time rather
    // than trusting a single wake -- a button press or any other interrupt can
    // wake us early. Bounded so a wake source that never fires cannot spin
    // here forever.
    for (int i = 0; i < 8; i++) {
        if ((am_hal_stimer_counter_get() - t0) >= ticks) break;
#if SCAN_IDLE_MODE == 2
        am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP);
#else
        am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_NORMAL);
#endif
    }

    // Whatever is left gets spun out. This is the safety net: if sleep never
    // wakes us, this degrades to the old busy-wait -- the pixel still gets its
    // full settling time and the scan still finishes. Slow, never wrong, and
    // never hung. If sleep is working this loop exits immediately.
    //
    // A tick of slack: waking has real latency, so landing a hair early is
    // normal and is not what we want to count as a failure.
    if ((am_hal_stimer_counter_get() - t0) + 6u < ticks) g_pwr_spun++;
    while ((am_hal_stimer_counter_get() - t0) < ticks) {
    }
#endif
}

void pwr_report(void) {
    flush();
#if BISEN_ENABLE_SWO_LOGGING
    uint64_t total_nj = pwr_frame_nj();
    uint32_t total_us = 0;
    for (int i = 0; i < PWR_COUNT; i++) total_us += ticks_to_us(s_tk[i]);
    if (total_us == 0 || total_nj == 0) return;

    am_util_stdio_printf("\nENERGY (core only, estimated from datasheet)\n");
    am_util_stdio_printf("  %-7s %9s %5s %7s %10s %5s\n",
                         "phase", "ms", "%", "uA", "uJ", "%");
    for (int i = 0; i < PWR_COUNT; i++) {
        if (s_tk[i] == 0) continue;
        uint32_t us = ticks_to_us(s_tk[i]);
        uint64_t nj = phase_nj(i);
        am_util_stdio_printf("  %-7s %6u.%03u %4u%% %7u %7u.%03u %4u%%\n",
                             s_name[i], us / 1000u, us % 1000u,
                             (unsigned)((uint64_t)us * 100u / total_us),
                             (unsigned)s_ua[i],
                             (unsigned)(nj / 1000u), (unsigned)(nj % 1000u),
                             (unsigned)(nj * 100u / total_nj));
    }
    am_util_stdio_printf("  %-7s %6u.%03u        %7s %7u.%03u\n", "TOTAL",
                         total_us / 1000u, total_us % 1000u, "",
                         (unsigned)(total_nj / 1000u),
                         (unsigned)(total_nj % 1000u));
    am_util_stdio_printf("  J/frame = %u.%06u mJ   (%u frames per joule)\n",
                         (unsigned)(total_nj / 1000000u),
                         (unsigned)(total_nj % 1000000u),
                         (unsigned)(1000000000ull / total_nj));
    am_util_stdio_printf("  excludes: analog front end, mux, ADC supply, LEDs,"
                         " J-Link\n");
    // Repeated every frame, not printed once at boot: opening the terminal
    // after the board has started must not hide whether these numbers mean
    // anything. Same reason the resume banner in infer.c repeats.
    if (!s_sleep_ok) {
        am_util_stdio_printf("  wake self-test FAILED -- spin-wait, mode %d "
                             "ignored\n", SCAN_IDLE_MODE);
    } else if (SCAN_IDLE_MODE == 0) {
        am_util_stdio_printf("  wake self-test passed -- sleeping is available"
                             " (build -DSCAN_IDLE_MODE=1 to use it)\n");
    } else {
        am_util_stdio_printf("  sleep mode %d: %u waits, %u fell back to"
                             " spinning -- %s\n", SCAN_IDLE_MODE,
                             (unsigned)g_pwr_waits, (unsigned)g_pwr_spun,
                             (g_pwr_spun * 20u > g_pwr_waits)
                                 ? "NOT SLEEPING, figures above are wrong"
                                 : "sleeping");
    }
#endif
    g_pwr_waits = 0;
    g_pwr_spun = 0;
}
