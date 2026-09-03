// energy_source.cc -- see energy_source.h. One source is compiled in.
//
// NOT a replacement for bisen_energy.cc -- ES_SOURCE_VCAP dispatches to it.
// See the header for why a selector is needed at all.
#include <math.h>

#include "energy_source.h"

#include "bisen/bisen_config.h"      // board conversion + unchanged thresholds

#if ES_SOURCE == ES_SOURCE_VCAP || ES_SOURCE == ES_SOURCE_BATT
#include "adc_shared.h"
#include "bisen/bisen_config.h"
#endif
// SIM needs STIMER for elapsed time; the others need the HAL anyway.
#include "am_mcu_apollo.h"

namespace {

#if ES_SOURCE != ES_SOURCE_BATT
// The direct-VDD diagnostic deliberately has no policy thresholds yet. Other
// source variants retain the existing camera application's level conversion.
es_level_t level_from_mv(uint32_t mv) {
    if (mv < bisen::kBenchEnergyPolicy.sleep_below_vcap_millivolts) {
        return ES_LEVEL_CRITICAL;
    }
    if (mv < bisen::kBenchEnergyPolicy.chunk_100_min_vcap_millivolts) {
        return ES_LEVEL_LOW;
    }
    return ES_LEVEL_OK;
}
#endif

es_reading_t g_last;

}  // namespace

// ---------------------------------------------------------------------------
#if ES_SOURCE == ES_SOURCE_SIM

// A MODELLED CAPACITOR, not a ramp.
//
// The earlier version dropped a fixed number of millivolts per READ, which is
// backwards: a capacitor loses energy to a LOAD over TIME, not to being looked
// at. That coupled the discharge rate to the polling interval, so changing
// PP_SCAN_POLL_PIXELS silently changed how often checkpoints fired.
//
// This tracks stored energy and derives the voltage from it:
//
//     E -= P * dt          the load draws constant power
//     V  = sqrt(2E / C)    what is left, as a voltage
//
// The sqrt matters. At constant power dV/dt = -P/(CV), so the rail falls
// FASTER as it empties -- which is exactly why the low bands are narrow in
// time and why a poll interval that was comfortable at 8 V can miss the
// checkpoint window at 5.9 V. A linear ramp hides that.
//
// Defaults model the bench rig in bisen_config.h -- 2 mF at 9 V -- drained by
// this application's own measured draw (4.09 mA at 1.9 V = 7.77 mW). That
// empties 9.0 V to 5.4 V in about 6.7 s, or roughly every 1.5 frames, which is
// frequent enough to watch on a scope. Raise ES_SIM_DRAW_UW to make it rarer.
//
// It also lets warning_us be real for the first time: the energy between here
// and the sleep floor, divided by the draw. That is what es_write_fits() needs
// to answer "can this write still finish".
#ifndef ES_SIM_C_UF
#define ES_SIM_C_UF 2000.0f      /* 2 mF, his bench capacitor */
#endif
#ifndef ES_SIM_V_START
#define ES_SIM_V_START 9.0f
#endif
#ifndef ES_SIM_V_FLOOR
#define ES_SIM_V_FLOOR 5.4f      /* below the 5.5 V sleep floor: recharge here */
#endif
#ifndef ES_SIM_DRAW_UW
#define ES_SIM_DRAW_UW 7771.0f   /* 4090 uA x 1.9 V, this app's ACTIVE draw */
#endif
#ifndef ES_SIM_SLEEP_UW
#define ES_SIM_SLEEP_UW 49.0f    /* 26 uA x 1.9 V, deep sleep -- 158x less */
#endif
// A harvester trickling in. Set between the sleep draw and the active draw and
// the system behaves the way the paper describes: computing drains, waiting in
// Stop recovers, and the rail oscillates around Th_safe instead of running
// monotonically flat until it dies. Zero models a dead harvester.
#ifndef ES_SIM_TRICKLE_UW
#define ES_SIM_TRICKLE_UW 1200.0f
#endif

static float    s_energy_uj;
static uint32_t s_last_tk;
static uint8_t  s_primed;
static float    s_load_uw = ES_SIM_DRAW_UW;   // updated by es_set_load_uw()

static float energy_at(float volts) {
    // 0.5*C*V^2, with C in microfarads, giving microjoules directly.
    return 0.5f * ES_SIM_C_UF * volts * volts;
}

int es_init(void) {
    s_energy_uj = energy_at(ES_SIM_V_START);
    s_last_tk = am_hal_stimer_counter_get();
    s_primed = 1;
    return 0;
}

void es_read(es_reading_t *out) {
    if (out == 0) return;
    if (!s_primed) (void)es_init();

    // STIMER runs at 6 MHz and is not in the debug power domain, so it keeps
    // counting through sleep -- which matters, because most of a frame is
    // spent asleep and a real capacitor keeps draining there. Unsigned
    // subtraction wraps correctly.
    const uint32_t now = am_hal_stimer_counter_get();
    const float dt_s = (float)(now - s_last_tk) / 6.0e6f;
    s_last_tk = now;

    // Net of what the machine is drawing right now and what the harvester is
    // putting back. In Stop the draw is ~158x lower, so the trickle wins and
    // the rail climbs -- which is what makes waiting worth doing.
    s_energy_uj += (ES_SIM_TRICKLE_UW - s_load_uw) * dt_s;
    const float full_uj = energy_at(ES_SIM_V_START);
    if (s_energy_uj > full_uj) s_energy_uj = full_uj;

    const float floor_uj = energy_at(ES_SIM_V_FLOOR);
    if (s_energy_uj <= floor_uj) {
        // Harvester reconnects. A real one would trickle rather than jump, but
        // the point of this source is to exercise the falling edge.
        s_energy_uj = energy_at(ES_SIM_V_START);
    }

    // V = sqrt(2E/C), with E in uJ and C in uF.
    const float volts = sqrtf(2.0f * s_energy_uj / ES_SIM_C_UF);
    const uint32_t mv = (uint32_t)(volts * 1000.0f + 0.5f);

    out->valid = 1;
    out->has_millivolts = 1;
    out->simulated = 1;
    out->millivolts = mv;
    out->level = level_from_mv(mv);
    out->raw_code = 0;

    // Time from here to the sleep floor at the modelled draw. Real, unlike the
    // ramp's 0, so es_write_fits() can actually refuse a write that cannot
    // complete.
    const float sleep_uj =
        energy_at((float)bisen::kBenchEnergyPolicy.sleep_below_vcap_millivolts
                  / 1000.0f);
    const float spare_uj = s_energy_uj - sleep_uj;
    // At the CURRENT draw, not the active one: how long this state can last.
    const float net_uw = s_load_uw - ES_SIM_TRICKLE_UW;
    out->warning_us = (spare_uj > 0.0f && net_uw > 0.0f)
                          ? (uint32_t)(spare_uj / net_uw * 1.0e6f)
                          : 0u;   /* 0 also means "not draining" -- see es_write_fits */
    g_last = *out;
}

void es_set_load_uw(uint32_t microwatts) { s_load_uw = (float)microwatts; }

uint32_t es_read_cost_us(void) { return 0u; }
const char *es_source_name(void) { return "modelled capacitor"; }

// ---------------------------------------------------------------------------
#elif ES_SOURCE == ES_SOURCE_VCAP

#ifndef BISEN_CAMERA_VCAP_IGNORE_AT_MV
#define BISEN_CAMERA_VCAP_IGNORE_AT_MV 9000u
#endif

// The divider on GPIO16/SE3, read through adc_shared.c.
//
// WHY THIS NO LONGER CALLS bisen::measure_energy()
// Not because anything is wrong with it -- because it insists on OWNING the
// ADC. It calls am_hal_adc_initialize(0, ...) on entry and
// am_hal_adc_deinitialize() on exit, on the same instance the camera holds for
// the life of the program. The first call fails, so this source never once
// produced a reading; if it had succeeded, the shutdown would have destroyed
// the camera's ADC.
//
// So the conversion is done by the single ADC owner instead, and the raw code
// is converted to millivolts by calibrated_vcap_millivolts_from_code(). The
// AMAP4PEVB default is the nominal direct GPIO16 divider conversion, not the
// incompatible Blue KXR fit. Policy thresholds are unchanged. Fit the two
// compile-time coefficients against DMM readings before research measurements.

int es_init(void) {
    // adc_shared_init() is called from sensor_init(), which runs first. There is
    // nothing left for this source to set up: the pad is configured, the slot
    // template exists, and the divider's one-time RC settle has been paid.
    uint32_t code;
    return adc_shared_read_vcap(&code) == 0 ? 0 : -1;
}

void es_read(es_reading_t *out) {
    if (out == 0) return;
    uint32_t code = 0;
    if (adc_shared_read_vcap(&code) != 0) {
        out->valid = 0;
        out->level = ES_LEVEL_UNKNOWN;
        return;
    }
    const uint32_t mv = bisen::calibrated_vcap_millivolts_from_code(code);

    // The 9 V bench point sits close enough to the old 9.04 V validation
    // ceiling that ADC dispersion can alternately accept and reject readings.
    // The user-defined camera policy is to use neither case: a sample at or
    // above 9.000 V is explicitly discarded and the policy layer immediately
    // asks for the next sample below the cutoff. Keep the raw measurement for
    // diagnostics, but do not let it select an energy band.
    if (mv >= BISEN_CAMERA_VCAP_IGNORE_AT_MV) {
        out->valid = 0;
        out->discard_and_retry = 1;
        out->has_millivolts = 1;
        out->simulated = 0;
        out->millivolts = mv;
        out->level = ES_LEVEL_UNKNOWN;
        out->raw_code = code;
        out->warning_us = 0;
        return;
    }

    // His independent physical-range check, kept: a code that implies a voltage
    // beyond what the divider can produce means something is wrong with the
    // front end, and a policy must not act on it.
    const float adc_volts =
        ((float)code / AM_HAL_ADC_SAMPLE_DIVISORF) * bisen::kAdcReferenceVolts;
    if (adc_volts > bisen::kVcapAdcMaxValidationVolts ||
        (adc_volts * bisen::kVcapDividerScale) > bisen::kVcapMaxValidationVolts) {
        out->valid = 0;
        out->level = ES_LEVEL_UNKNOWN;
        return;
    }

    out->valid = 1;
    out->has_millivolts = 1;
    out->simulated = 0;
    out->millivolts = mv;
    out->level = level_from_mv(mv);
    out->raw_code = code;
    out->warning_us = 0;
    g_last = *out;
}

// Two averaged conversions and two mode changes. The 5 ms divider settle that
// used to dominate this number is gone: the pad is configured once at boot and
// never reconfigured, so the node is already settled. That is what makes
// sampling inside an inference affordable at all.
uint32_t es_read_cost_us(void) { return adc_shared_vcap_cost_us(); }
void es_set_load_uw(uint32_t microwatts) { (void)microwatts; }
const char *es_source_name(void) {
    return bisen::kVcapCalibrationBenchValidated
               ? "VCAP J9.8/GPIO16/ADCSE3 (bench-calibrated)"
               : "VCAP J9.8/GPIO16/ADCSE3 (nominal, UNCALIBRATED)";
}

// ---------------------------------------------------------------------------
#elif ES_SOURCE == ES_SOURCE_BATT

// The internal AM_HAL_ADC_SLOT_CHSEL_BATT channel: an on-chip divider, no
// external parts, no pin.
//
// This target intentionally powers VDD_MCU directly, so the internal divided
// rail is the energy source of interest rather than a regulator output. It
// still shares ADC0 with the camera photodiode through adc_shared.c.
int es_init(void) {
    uint32_t code = 0u;
    return adc_shared_read_supply(&code) == 0 ? 0 : -1;
}

void es_read(es_reading_t *out) {
    if (out == 0) return;
    uint32_t code = 0u;
    if (adc_shared_read_supply(&code) != 0) {
        out->valid = 0;
        out->level = ES_LEVEL_UNKNOWN;
        return;
    }

    out->valid = 0;
    out->discard_and_retry = 0;
    out->has_millivolts = 1;
    out->simulated = 0;
    out->millivolts = adc_shared_supply_nominal_millivolts(code);
    out->level = ES_LEVEL_UNKNOWN;
    out->warning_us = 0u;
    out->raw_code = code;
}

uint32_t es_read_cost_us(void) { return adc_shared_supply_cost_us(); }
void es_set_load_uw(uint32_t microwatts) { (void)microwatts; }
const char *es_source_name(void) {
    return "VDD_MCU via internal BATT (VDD/3), diagnostic-only";
}

// ---------------------------------------------------------------------------
#elif ES_SOURCE == ES_SOURCE_PIN

// A power-management IC or comparator asserting a GPIO when the supply falls
// past its threshold. This is what BISen's own hardware does -- their paper
// describes "two external interrupts triggered by intelligent power
// management" -- and it is the cheapest of all the options at run time: an
// interrupt sets a flag, and reading it costs nothing.
//
// It reports no voltage, so the band policy cannot run. The level alone drives
// the decision, which is why es_reading_t carries a level independent of
// millivolts.
//
// ⚠ NOT WIRED. Needs: a free GPIO, an ISR setting s_asserted, and a figure for
// warning_us taken from the comparator's threshold and the capacitor's reserve.
// Until then es_init() refuses.
static volatile uint8_t s_asserted;

int es_init(void) { return -1; }

void es_read(es_reading_t *out) {
    if (out == 0) return;
    out->valid = 0;
    out->has_millivolts = 0;
    out->level = s_asserted ? ES_LEVEL_LOW : ES_LEVEL_OK;
}

uint32_t es_read_cost_us(void) { return 0u; }
void es_set_load_uw(uint32_t microwatts) { (void)microwatts; }
const char *es_source_name(void) { return "PMU interrupt pin"; }

#else
#error "ES_SOURCE must be one of ES_SOURCE_SIM / _VCAP / _BATT / _PIN"
#endif

// ---------------------------------------------------------------------------

int es_write_fits(uint32_t write_us) {
    // An unknown budget must not suppress checkpoints. Refusing to save because
    // we cannot prove there is time loses the work outright, whereas attempting
    // it and being cut off leaves the previous checkpoint intact -- the payload
    // is written before the header, so a torn write is already survivable.
    if (!g_last.valid || g_last.warning_us == 0u) return 1;
    return write_us <= g_last.warning_us;
}
