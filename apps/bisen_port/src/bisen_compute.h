#ifndef BISEN_COMPUTE_H
#define BISEN_COMPUTE_H

#include "bisen_config.h"

namespace bisen {

enum class ComputeStatus : uint8_t {
    kProgress = 0,
    kComplete,
    kGoldenMismatch,
    kInvalidContext,
};

ComputeStatus run_compute_chunk(Context *context, uint32_t chunk_pixels);
bool sobel_context_is_valid(const Context &context);
uint32_t sobel_golden_digest();
uint32_t sobel_total_pixels();

}  // namespace bisen

#endif  // BISEN_COMPUTE_H
