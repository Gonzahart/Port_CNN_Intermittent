// External trace adapter; the board itself stays on a steady DC supply.
#include "energy_source.h"
#include "adc_shared.h"
#include "trace_input.h"
#if ES_SOURCE != ES_SOURCE_TRACE
#error "This app only supports the external trace input"
#endif
int es_init(void) {
    uint32_t code = 0;
    return adc_shared_read_supply(&code);
}
void es_read(es_reading_t *out) {
    if (!out) return;
    *out = {};
    uint32_t code = 0;
    if (adc_shared_read_supply(&code) != 0 || code > 4095u) return;
    const uint32_t uv = trace_input_microvolts(code);
    out->valid = 1;
    out->has_millivolts = 1;
    out->simulated = 1;  // physical measurement, artificial energy availability
    out->millivolts = uv / 1000u;
    out->raw_code = trace_policy_code(uv);
    out->level = uv < 1900000u ? ES_LEVEL_CRITICAL :
                 uv < 2000000u ? ES_LEVEL_LOW : ES_LEVEL_OK;
    out->warning_us = 0;  // no actual time-to-power-failure estimate
}
uint32_t es_read_cost_us(void) { return adc_shared_supply_cost_us(); }
void es_set_load_uw(uint32_t microwatts) { (void)microwatts; }
const char *es_source_name(void) {
    return "EMULATED energy: J9.8/GPIO16/ADCSE3, independent DC board supply";
}
int es_write_fits(uint32_t write_us) { (void)write_us; return 1; }
