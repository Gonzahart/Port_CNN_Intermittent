//
// adc_shared.c - single owner of Apollo4 ADC instance 0. See adc_shared.h for
// why this exists and why the modes are exclusive rather than concurrent.
//
// Compiled as C so the AmbiqSuite designated-initializer macros work, exactly
// as sensor.c is.
//
#include "adc_shared.h"
#include "energy_source.h"
#include "power.h"
#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

// Pixel readout: photodiode on GPIO15, hard-wired to ADC channel SE4.
#define PIXEL_PIN      15
#define PIXEL_CHANNEL  AM_HAL_ADC_SLOT_CHSEL_SE4
#define PIXEL_SLOT     0

// Supply divider on AMAP4PEVB J9.8/GPIO16, hard-wired to ADCSE3. GPIO15/SE4
// remains the photodiode input.
#define VCAP_PIN       16
#define VCAP_CHANNEL   AM_HAL_ADC_SLOT_CHSEL_SE3
#define VCAP_SLOT      1

#ifndef BISEN_CAMERA_VCAP_GPIO16_CONFIRMED
#define BISEN_CAMERA_VCAP_GPIO16_CONFIRMED 0
#endif

#if ES_SOURCE == ES_SOURCE_VCAP && !BISEN_CAMERA_VCAP_GPIO16_CONFIRMED
#error "Physical camera VCAP sensing requires the divider on AMAP4PEVB J9.8/GPIO16/ADCSE3"
#endif

// The divider's 10 nF needs several RC time constants after the pad is first
// configured as an analog input. Paid ONCE here, not per reading -- the pad is
// configured at boot and never reconfigured, so the node stays settled.
#define VCAP_FIRST_SETTLE_US  5000u

// ERR091/ERR113: >= 37 tracking cycles required. 63 is the field maximum and
// is what the divider's ~52.9 kohm Thevenin source resistance wants anyway.
#define TRACKING_CYCLES  63u

static void    *g_h;
static adc_mode_t g_mode = ADC_MODE_PIXEL;
static int       g_watch_fired;
static int       g_watch_self_repeats;
static uint32_t  g_watch_last_code;

// ---------------------------------------------------------------------------

static void apply_config(int low_power_mode_1, int repeating) {
    am_hal_adc_config_t cfg = {
        // ERR091/ERR113: 24 MHz HFRC is the only supported setting.
        .eClock     = AM_HAL_ADC_CLKSEL_HFRC_24MHZ,
        .eRepeatTrigger = AM_HAL_ADC_RPTTRIGSEL_TMR,
        .ePolarity  = AM_HAL_ADC_TRIGPOL_RISING,
        .eTrigger   = AM_HAL_ADC_TRIGSEL_SOFTWARE,
        .eClockMode = AM_HAL_ADC_CLKMODE_LOW_POWER,
        // LPMODE0 keeps the ADC ready between scans: 0 us to start a scan, but
        // it burns power doing so. LPMODE1 powers down between scans and costs
        // 53.7 us to restart (datasheet p.213). The camera fires 1024 scans
        // back to back, so LPMODE0 saves it ~55 ms per frame; the supply is read
        // rarely, so LPMODE1 is the right trade there.
        .ePowerMode = low_power_mode_1 ? AM_HAL_ADC_LPMODE1 : AM_HAL_ADC_LPMODE0,
        .eRepeat    = repeating ? AM_HAL_ADC_REPEATING_SCAN
                                : AM_HAL_ADC_SINGLE_SCAN,
    };
    (void)am_hal_adc_configure(g_h, &cfg);
}

static void apply_slots(adc_mode_t mode) {
    // Exactly one slot is ever enabled, so a scan produces exactly one FIFO
    // entry and there is nothing to demux.
    am_hal_adc_slot_config_t pixel = {
        .bEnabled       = (mode == ADC_MODE_PIXEL),
        .bWindowCompare = false,
        .eChannel       = PIXEL_CHANNEL,
        .eMeasToAvg     = AM_HAL_ADC_SLOT_AVG_16,
        .ePrecisionMode = AM_HAL_ADC_SLOT_12BIT,
        .ui32TrkCyc     = TRACKING_CYCLES,
    };
    am_hal_adc_slot_config_t vcap = {
        .bEnabled       = (mode != ADC_MODE_PIXEL),
        // Only the watcher needs the comparator. Leaving it off for a plain
        // reading keeps the window registers meaningless when nothing is armed.
        .bWindowCompare = (mode == ADC_MODE_WATCH),
        .eChannel       = VCAP_CHANNEL,
        .eMeasToAvg     = AM_HAL_ADC_SLOT_AVG_16,
        .ePrecisionMode = AM_HAL_ADC_SLOT_12BIT,
        .ui32TrkCyc     = TRACKING_CYCLES,
    };
    (void)am_hal_adc_configure_slot(g_h, PIXEL_SLOT, &pixel);
    (void)am_hal_adc_configure_slot(g_h, VCAP_SLOT,  &vcap);
}

static void enter_mode(adc_mode_t mode) {
    (void)am_hal_adc_disable(g_h);
    apply_config(mode != ADC_MODE_PIXEL, mode == ADC_MODE_WATCH);
    apply_slots(mode);
    (void)am_hal_adc_enable(g_h);
    g_mode = mode;
}

// Drain whatever is sitting in the FIFO. Stale entries from a previous mode
// would otherwise be read as if they were this mode's result.
static void fifo_drain(void) {
    while (AM_HAL_ADC_FIFO_COUNT(ADC->FIFO)) {
        uint32_t n = 1;
        am_hal_adc_sample_t s;
        (void)am_hal_adc_samples_read(g_h, true, NULL, &n, &s);
    }
}

// One averaged conversion, polled. AVG_16 means the hardware needs 16 scans
// before it emits a result: SCNCMP fires after each, CNVCMP only after the
// last. This is the loop sensor.c has always used, moved here unchanged in
// behaviour.
//
// Returns the raw sample word, or 0xFFFFFFFF if it never completed.
static uint32_t convert_once(void) {
    uint32_t st;
    (void)am_hal_adc_interrupt_status(g_h, &st, false);
    (void)am_hal_adc_interrupt_clear(g_h, st);
    fifo_drain();

    // Bounded, unlike an open `while (1)`: AVG_16 needs exactly 16 scans, and a
    // dead peripheral must not hang the frame. The margin covers a scan lost to
    // a mode change landing mid-flight.
    for (uint32_t attempt = 0; attempt < 24u; attempt++) {
        (void)am_hal_adc_sw_trigger(g_h);
        do {
            (void)am_hal_adc_interrupt_status(g_h, &st, false);
        } while (!(st & AM_HAL_ADC_INT_SCNCMP));
        (void)am_hal_adc_interrupt_clear(g_h, st);
        if (st & AM_HAL_ADC_INT_CNVCMP) {
            am_util_delay_us(30);
            uint32_t n = 1, val;
            am_hal_adc_sample_t s;
            (void)am_hal_daxi_control(AM_HAL_DAXI_CONTROL_INVALIDATE, NULL);
            (void)am_hal_adc_samples_read(g_h, true, NULL, &n, &s);
            val = AM_HAL_ADC_FIFO_SAMPLE(s.ui32Sample);
            return val;
        }
    }
    return 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------

void adc_shared_init(void) {
    am_hal_gpio_pincfg_t pixel_pad = { .GP.cfg_b.uFuncSel = AM_HAL_PIN_15_ADCSE4 };
    (void)am_hal_gpio_pinconfig(PIXEL_PIN, pixel_pad);
#if ES_SOURCE == ES_SOURCE_VCAP
    am_hal_gpio_pincfg_t vcap_pad  = { .GP.cfg_b.uFuncSel = AM_HAL_PIN_16_ADCSE3 };
    (void)am_hal_gpio_pinconfig(VCAP_PIN,  vcap_pad);
#endif

    (void)am_hal_adc_initialize(0, &g_h);
    (void)am_hal_adc_power_control(g_h, AM_HAL_SYSCTRL_WAKE, false);
    enter_mode(ADC_MODE_PIXEL);

    // The one and only divider settle. Everything after this reads a node that
    // has been sitting at its final voltage since boot, which is what turns a
    // 5.2 ms supply reading into a ~0.2 ms one.
    am_util_delay_us(VCAP_FIRST_SETTLE_US);
}

adc_mode_t adc_shared_mode(void) { return g_mode; }

void adc_shared_set_mode(adc_mode_t mode) {
    if (mode != g_mode) enter_mode(mode);
}

uint32_t adc_shared_read_pixel(void) {
    if (g_mode != ADC_MODE_PIXEL) enter_mode(ADC_MODE_PIXEL);
    uint32_t v = convert_once();
    return (v == 0xFFFFFFFFu) ? 0u : v;
}

int adc_shared_read_vcap(uint32_t *out_code) {
    if (out_code == 0) return -1;
    const adc_mode_t prev = g_mode;

    enter_mode(ADC_MODE_VCAP);
    // First conversion after a mode change sees the reference and sample/hold
    // still starting up. Discarding it is the same precaution the original
    // energy path took, and it costs one conversion rather than 5 ms.
    (void)convert_once();
    const uint32_t code = convert_once();
    if (prev != ADC_MODE_VCAP) enter_mode(prev);

    if (code == 0xFFFFFFFFu) return -1;
    *out_code = code;
    return 0;
}

uint32_t adc_shared_vcap_cost_us(void) {
    // Two averaged conversions plus two mode changes. No divider settle: that
    // was paid at boot. Measured behaviour should be checked against this.
    return 2u * 107u + 20u;
}

// ---------------------------------------------------------------------------

int adc_shared_watch_arm(uint32_t lower_code, uint32_t upper_code) {
    g_watch_fired = 0;
    g_watch_self_repeats = 0;
    g_watch_last_code = 0;

    enter_mode(ADC_MODE_WATCH);

    // Arm the band the rail is currently inside. Leaving it upward means the
    // supply recovered; leaving it downward means it is still collapsing and a
    // checkpoint is due. One comparator, both answers -- which is why the wait
    // state does not need to keep taking readings to stay safe.
    //
    // The limits are 20-bit registers (WULIM/WLLIM), wide enough for the 12-bit
    // code plus the 6 fractional bits hardware averaging produces, so the
    // comparison is against the AVERAGED value rather than a single raw
    // conversion. That matters: a single conversion on a noisy rail would
    // trip early.
    am_hal_adc_window_config_t win = {
        .bScaleLimits = false,
        .ui32Upper    = upper_code,
        .ui32Lower    = lower_code,
    };
    if (am_hal_adc_control(g_h, AM_HAL_ADC_REQ_WINDOW_CONFIG, &win)
            != AM_HAL_STATUS_SUCCESS) {
        enter_mode(ADC_MODE_PIXEL);
        return -1;
    }

    uint32_t st;
    (void)am_hal_adc_interrupt_status(g_h, &st, false);
    (void)am_hal_adc_interrupt_clear(g_h, st);
    fifo_drain();

    // Kick the first scan. In repeating mode the peripheral should keep going
    // from here without further triggers; whether it actually does is measured
    // rather than assumed, just below.
    if (am_hal_adc_sw_trigger(g_h) != AM_HAL_STATUS_SUCCESS) {
        enter_mode(ADC_MODE_PIXEL);
        return -1;
    }

    // Did a scan complete unprompted? If yes the ADC is genuinely free-running
    // and the wait costs nothing but a status read per poll. If no, the caller
    // still gets a working watcher -- adc_shared_watch_fired() re-triggers --
    // it just is not autonomous. Either way the answer is reported rather than
    // guessed.
    for (uint32_t i = 0; i < 4u; i++) {
        am_util_delay_us(200);
        (void)am_hal_adc_interrupt_status(g_h, &st, false);
        if (st & AM_HAL_ADC_INT_SCNCMP) { g_watch_self_repeats = 1; break; }
    }
    return 0;
}

int adc_shared_watch_fired(void) {
    if (g_mode != ADC_MODE_WATCH) return 0;
    if (g_watch_fired) return 1;

    uint32_t st = 0;
    (void)am_hal_adc_interrupt_status(g_h, &st, false);

    // WCEXC is the excursion flag: the averaged value has left the armed band.
    // WCINC (incursion) is watched too and treated identically -- the datasheet
    // describes the comparator as monitoring excursions "into or out of" the
    // window, and the arming above starts with the value already inside, so a
    // crossing is the event whichever flag the part chooses to raise.
    if (st & (AM_HAL_ADC_INT_WCINC | AM_HAL_ADC_INT_WCEXC)) {
        g_watch_fired = 1;
    }

    // Keep the last value visible for the log, and keep the FIFO from filling
    // in repeating mode -- 16 entries is about a second of scans.
    if (st & AM_HAL_ADC_INT_SCNCMP) {
        while (AM_HAL_ADC_FIFO_COUNT(ADC->FIFO)) {
            uint32_t n = 1;
            am_hal_adc_sample_t s;
            (void)am_hal_daxi_control(AM_HAL_DAXI_CONTROL_INVALIDATE, NULL);
            (void)am_hal_adc_samples_read(g_h, true, NULL, &n, &s);
            g_watch_last_code = AM_HAL_ADC_FIFO_SAMPLE(s.ui32Sample);
        }
    }
    (void)am_hal_adc_interrupt_clear(g_h, st);

    // Not free-running: this poll has to ask for the next scan itself.
    if (!g_watch_self_repeats) (void)am_hal_adc_sw_trigger(g_h);

    return g_watch_fired;
}

uint32_t adc_shared_watch_last_code(void) { return g_watch_last_code; }
int adc_shared_watch_self_repeats(void)   { return g_watch_self_repeats; }

void adc_shared_watch_disarm(void) {
    if (g_mode == ADC_MODE_WATCH) enter_mode(ADC_MODE_PIXEL);
    g_watch_fired = 0;
}
