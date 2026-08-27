#ifndef BISEN_RUNTIME_H
#define BISEN_RUNTIME_H

#include "bisen_config.h"

namespace bisen {

// Volatile execution state retained by MCU TCM across normal RTC deep sleep.
// It is explicitly initialized on every boot/reset; full-power recovery still
// comes only from the validated two-slot MRAM checkpoint.
struct alignas(8) BisenRuntimeContext {
    Context context;
    uint32_t runtime_generation;
    uint32_t committed_generation;
    uint32_t current_chunk_pixels;
    uint32_t checkpoint_attempt_count;
    uint32_t checkpoint_success_count;
    uint32_t checkpoint_failure_latched;
};

static_assert(sizeof(BisenRuntimeContext) == 56u,
              "Retained runtime-context size changed; update map/retention evidence");

BisenRuntimeContext *runtime_context();
void runtime_context_reset();
void runtime_context_accept_restore(uint32_t checkpoint_sequence);
void runtime_context_note_recovery_change();
void runtime_context_note_job_complete_without_checkpoint();
bool runtime_context_is_dirty();
bool runtime_context_begin_checkpoint_attempt();
void runtime_context_note_checkpoint_success();
void runtime_context_note_checkpoint_failure();
bool runtime_context_checkpoint_failed();

}  // namespace bisen

#endif  // BISEN_RUNTIME_H
