#include "harvest_stats.h"
#include "am_mcu_apollo.h"

volatile harvest_stats_t g_harvest_stats;
static uint8_t s_state = 0xffu;
static uint32_t s_last_tick;

void harvest_stats_note_state(uint8_t code) {
    if (code > 7u) return;
    const uint32_t now = am_hal_stimer_counter_get();
    if (s_state <= 7u) {
        g_harvest_stats.state_ticks[s_state] += (uint32_t)(now - s_last_tick);
    }
    if (s_state != code) g_harvest_stats.state_entries[code]++;
    s_state = code;
    s_last_tick = now;
}

void harvest_stats_note_observation(void) {
    g_harvest_stats.vcap_observations++;
}

void harvest_stats_note_camera(void) {
    g_harvest_stats.camera_activations++;
}

void harvest_stats_note_chunk(uint32_t budget) {
    if (budget == 100u) g_harvest_stats.chunks_100++;
    else if (budget == 500u) g_harvest_stats.chunks_500++;
    else if (budget == 1000u) g_harvest_stats.chunks_1000++;
}

void harvest_stats_snapshot(harvest_stats_t *out) {
    if (!out) return;
    const uint32_t now = am_hal_stimer_counter_get();
    for (uint32_t i = 0; i < 8u; ++i) {
        out->state_ticks[i] = g_harvest_stats.state_ticks[i];
        out->state_entries[i] = g_harvest_stats.state_entries[i];
    }
    if (s_state <= 7u) {
        out->state_ticks[s_state] += (uint32_t)(now - s_last_tick);
    }
    out->vcap_observations = g_harvest_stats.vcap_observations;
    out->camera_activations = g_harvest_stats.camera_activations;
    out->chunks_100 = g_harvest_stats.chunks_100;
    out->chunks_500 = g_harvest_stats.chunks_500;
    out->chunks_1000 = g_harvest_stats.chunks_1000;
}
