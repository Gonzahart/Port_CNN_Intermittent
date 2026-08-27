#include <assert.h>

#include "bisen_compute.h"

namespace {

bisen::Context finish_with_chunks(uint32_t chunk_pixels) {
    bisen::Context context = bisen::new_context();
    bisen::ComputeStatus status = bisen::ComputeStatus::kProgress;
    while (status == bisen::ComputeStatus::kProgress) {
        assert(bisen::sobel_context_is_valid(context));
        status = bisen::run_compute_chunk(&context, chunk_pixels);
        if (status == bisen::ComputeStatus::kComplete) {
            context.completion_mask |= bisen::kComputeComplete;
            context.job_complete = true;
        }
    }
    assert(status == bisen::ComputeStatus::kComplete);
    assert(bisen::sobel_context_is_valid(context));
    return context;
}

}  // namespace

int main() {
    const bisen::Context one_shot = finish_with_chunks(bisen::sobel_total_pixels());
    const bisen::Context chunk_100 = finish_with_chunks(100u);
    const bisen::Context chunk_500 = finish_with_chunks(500u);
    const bisen::Context chunk_1000 = finish_with_chunks(1000u);
    assert(one_shot.compute_index == bisen::sobel_total_pixels());
    assert(one_shot.workload_digest == bisen::sobel_golden_digest());
    assert(chunk_100.workload_digest == one_shot.workload_digest);
    assert(chunk_500.workload_digest == one_shot.workload_digest);
    assert(chunk_1000.workload_digest == one_shot.workload_digest);

    bisen::Context before_reset = bisen::new_context();
    assert(bisen::run_compute_chunk(&before_reset, 500u) ==
           bisen::ComputeStatus::kProgress);
    const bisen::Context persisted_copy = before_reset;
    bisen::Context resumed = persisted_copy;
    while (resumed.compute_index < bisen::sobel_total_pixels()) {
        const bisen::ComputeStatus status =
            bisen::run_compute_chunk(&resumed, 500u);
        if (status == bisen::ComputeStatus::kComplete) {
            resumed.completion_mask |= bisen::kComputeComplete;
            resumed.job_complete = true;
        } else {
            assert(status == bisen::ComputeStatus::kProgress);
        }
    }
    assert(resumed.workload_digest == one_shot.workload_digest);

    bisen::Context corrupt = bisen::new_context();
    corrupt.output_progress = 1u;
    assert(!bisen::sobel_context_is_valid(corrupt));
    assert(bisen::run_compute_chunk(&corrupt, 100u) ==
           bisen::ComputeStatus::kInvalidContext);
    return 0;
}
