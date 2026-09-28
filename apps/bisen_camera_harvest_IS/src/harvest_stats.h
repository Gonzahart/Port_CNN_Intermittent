#ifndef BISEN_HARVEST_STATS_H
#define BISEN_HARVEST_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t state_ticks[8];       // STIMER ticks at 6 MHz; divide by six for us
    uint32_t state_entries[8];
    uint32_t vcap_observations;
    uint32_t camera_activations;
    uint32_t chunks_100;
    uint32_t chunks_500;
    uint32_t chunks_1000;
} harvest_stats_t;

extern volatile harvest_stats_t g_harvest_stats;
void harvest_stats_note_state(uint8_t code);
void harvest_stats_note_observation(void);
void harvest_stats_note_camera(void);
void harvest_stats_note_chunk(uint32_t budget);
void harvest_stats_snapshot(harvest_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
