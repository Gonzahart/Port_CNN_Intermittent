#include "bisen_runtime.h"

namespace {

#if defined(BISEN_HOST_TEST)
__attribute__((aligned(8)))
#else
__attribute__((section(".bisen_retained"), aligned(8)))
#endif
bisen::BisenRuntimeContext g_bisen_runtime;

}  // namespace

namespace bisen {

BisenRuntimeContext *runtime_context() { return &g_bisen_runtime; }

void runtime_context_reset() {
    g_bisen_runtime = {};
    g_bisen_runtime.context = new_context();
}

void runtime_context_accept_restore(uint32_t checkpoint_sequence) {
    // Seed the session generations from the persistent sequence so a restored
    // record begins clean without persisting a separate wear counter.
    g_bisen_runtime.runtime_generation = checkpoint_sequence;
    g_bisen_runtime.committed_generation = checkpoint_sequence;
    g_bisen_runtime.checkpoint_failure_latched = 0u;
}

void runtime_context_note_recovery_change() {
    uint32_t next = g_bisen_runtime.runtime_generation + 1u;
    if (next == g_bisen_runtime.committed_generation) {
        ++next;
    }
    g_bisen_runtime.runtime_generation = next;
}

void runtime_context_note_job_complete_without_checkpoint() {
    // A completed deterministic workload has no incomplete progress that
    // needs recovery. Mark it clean in retained-RAM accounting only; this does
    // not program, invalidate, or otherwise modify the older MRAM checkpoint.
    g_bisen_runtime.committed_generation =
        g_bisen_runtime.runtime_generation;
}

bool runtime_context_is_dirty() {
    return g_bisen_runtime.runtime_generation !=
           g_bisen_runtime.committed_generation;
}

bool runtime_context_begin_checkpoint_attempt() {
    if (g_bisen_runtime.checkpoint_failure_latched != 0u ||
        g_bisen_runtime.checkpoint_attempt_count >=
            static_cast<uint32_t>(BISEN_MRAM_SESSION_ATTEMPT_LIMIT)) {
        return false;
    }
    ++g_bisen_runtime.checkpoint_attempt_count;
    return true;
}

void runtime_context_note_checkpoint_success() {
    ++g_bisen_runtime.checkpoint_success_count;
    g_bisen_runtime.committed_generation =
        g_bisen_runtime.runtime_generation;
    // Matching the MSP430, the successful save itself makes the recovery
    // context clean. With no useful work allowed at low VCAP, another write
    // cannot be requested until computation has produced new progress.
}

void runtime_context_note_checkpoint_failure() {
    g_bisen_runtime.checkpoint_failure_latched = 1u;
}

bool runtime_context_checkpoint_failed() {
    return g_bisen_runtime.checkpoint_failure_latched != 0u;
}

}  // namespace bisen
