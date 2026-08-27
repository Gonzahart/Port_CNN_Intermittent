#include <assert.h>
#include <stdio.h>

#include "bisen_runtime.h"

int main() {
    bisen::runtime_context_reset();
    bisen::BisenRuntimeContext *runtime = bisen::runtime_context();
    assert(runtime->context.workload_digest == bisen::kWorkloadInitialDigest);
    assert(!bisen::runtime_context_is_dirty());

    bisen::runtime_context_note_recovery_change();
    assert(runtime->runtime_generation == 1u);
    assert(bisen::runtime_context_is_dirty());
    assert(bisen::runtime_context_begin_checkpoint_attempt());
    bisen::runtime_context_note_checkpoint_success();
    assert(runtime->committed_generation == runtime->runtime_generation);
    assert(!bisen::runtime_context_is_dirty());
    assert(runtime->checkpoint_attempt_count == 1u);
    assert(runtime->checkpoint_success_count == 1u);

    bisen::runtime_context_note_recovery_change();
    assert(bisen::runtime_context_is_dirty());
    const uint32_t successful_checkpoints = runtime->checkpoint_success_count;
    runtime->context.job_complete = true;
    runtime->context.completion_mask |= bisen::kComputeComplete;
    bisen::runtime_context_note_job_complete_without_checkpoint();
    assert(!bisen::runtime_context_is_dirty());
    assert(runtime->checkpoint_success_count == successful_checkpoints);
    assert(runtime->context.job_complete);
    assert((runtime->context.completion_mask & bisen::kComputeComplete) != 0u);

    bisen::runtime_context_note_checkpoint_failure();
    assert(bisen::runtime_context_checkpoint_failed());
    assert(!bisen::runtime_context_begin_checkpoint_attempt());

    bisen::runtime_context_reset();
    for (uint32_t i = 0u;
         i < static_cast<uint32_t>(BISEN_MRAM_SESSION_ATTEMPT_LIMIT); ++i) {
        assert(bisen::runtime_context_begin_checkpoint_attempt());
        bisen::runtime_context_note_checkpoint_success();
    }
    assert(!bisen::runtime_context_begin_checkpoint_attempt());

    bisen::runtime_context_reset();
    bisen::runtime_context_accept_restore(23u);
    assert(runtime->runtime_generation == 23u);
    assert(runtime->committed_generation == 23u);
    assert(!bisen::runtime_context_is_dirty());

    puts("bisen_runtime_host_test: PASS");
    return 0;
}
