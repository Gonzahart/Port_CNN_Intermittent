// Physical harvested-energy reservoir ahead of the MP1584EN.
#include "energy_source.h"
#include "adc_shared.h"
#include "trace_input.h"
#include "harvest_stats.h"
#if ES_SOURCE != ES_SOURCE_HARVEST
#error "This app only supports the physical VCAP input"
#endif
int es_init(void) {
    uint32_t code = 0;
    return adc_shared_read_supply(&code);
}
void es_read(es_reading_t *out) {
    if (!out) return;
    *out = {};
    harvest_stats_note_observation();
    uint32_t code = 0;
    if (adc_shared_read_supply(&code) != 0 || code >= 4090u) return;
    const uint32_t uv = trace_input_microvolts(code);
    if (uv > BISEN_HARVEST_MAX_VCAP_UV) return;
    out->valid = 1;
    out->has_millivolts = 1;
    out->simulated = 0;
    out->millivolts = uv / 1000u;
    out->raw_code = trace_policy_code(uv);
    out->level = uv < BISEN_HARVEST_CRITICAL_UV ? ES_LEVEL_CRITICAL :
                 uv < BISEN_HARVEST_WORK100_UV ? ES_LEVEL_LOW : ES_LEVEL_OK;
    out->warning_us = 0;  // no actual time-to-power-failure estimate
}
uint32_t es_read_cost_us(void) { return adc_shared_supply_cost_us(); }
void es_set_load_uw(uint32_t microwatts) { (void)microwatts; }
const char *es_source_name(void) {
    return "PHYSICAL VCAP: J9.8/GPIO16/ADCSE3 ahead of MP1584EN";
}
int es_write_fits(uint32_t write_us) { (void)write_us; return 1; }
