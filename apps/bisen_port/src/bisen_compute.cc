#include "bisen_compute.h"

namespace bisen {
namespace {

constexpr uint32_t kImageWidth = 64u;
constexpr uint32_t kImageHeight = 64u;
constexpr uint32_t kOutputWidth = kImageWidth - 2u;
constexpr uint32_t kOutputHeight = kImageHeight - 2u;
constexpr uint32_t kFnvPrime = 16777619u;
constexpr uint32_t kGoldenDigest = 0x8bdd7454u;

uint8_t input_pixel(uint32_t x, uint32_t y) {
    return static_cast<uint8_t>((x * 17u + y * 31u + ((x * y) % 29u) * 7u + 13u) & 0xffu);
}

uint8_t sobel_pixel(uint32_t output_index) {
    const uint32_t x = (output_index % kOutputWidth) + 1u;
    const uint32_t y = (output_index / kOutputWidth) + 1u;
    const int32_t tl = input_pixel(x - 1u, y - 1u);
    const int32_t tc = input_pixel(x, y - 1u);
    const int32_t tr = input_pixel(x + 1u, y - 1u);
    const int32_t ml = input_pixel(x - 1u, y);
    const int32_t mr = input_pixel(x + 1u, y);
    const int32_t bl = input_pixel(x - 1u, y + 1u);
    const int32_t bc = input_pixel(x, y + 1u);
    const int32_t br = input_pixel(x + 1u, y + 1u);
    const int32_t gx = tl + 2 * ml + bl - tr - 2 * mr - br;
    const int32_t gy = tl + 2 * tc + tr - bl - 2 * bc - br;
    const int32_t magnitude = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
    return static_cast<uint8_t>(magnitude > 255 ? 255 : magnitude);
}

}  // namespace

ComputeStatus run_compute_chunk(Context *context, uint32_t chunk_pixels) {
    if (context == nullptr || chunk_pixels == 0u ||
        !sobel_context_is_valid(*context)) {
        return ComputeStatus::kInvalidContext;
    }
    const uint32_t total = sobel_total_pixels();
    uint32_t remaining = total - (context->compute_index < total ? context->compute_index : total);
    const uint32_t count = chunk_pixels < remaining ? chunk_pixels : remaining;
    for (uint32_t i = 0; i < count; ++i) {
        context->workload_digest ^= sobel_pixel(context->compute_index);
        context->workload_digest *= kFnvPrime;
        ++context->compute_index;
    }
    context->output_progress = context->compute_index;
    if (context->compute_index != total) {
        return ComputeStatus::kProgress;
    }
    return context->workload_digest == kGoldenDigest ? ComputeStatus::kComplete
                                                       : ComputeStatus::kGoldenMismatch;
}

bool sobel_context_is_valid(const Context &context) {
    const uint32_t total = sobel_total_pixels();
    const bool compute_complete =
        (context.completion_mask & static_cast<uint32_t>(kComputeComplete)) != 0u;
    if (context.compute_index > total ||
        context.output_progress != context.compute_index ||
        context.job_complete != compute_complete) {
        return false;
    }
    if (compute_complete) {
        return context.compute_index == total &&
               context.workload_digest == kGoldenDigest;
    }
    if (context.compute_index == total) {
        return false;
    }
    return context.compute_index != 0u ||
           context.workload_digest == kWorkloadInitialDigest;
}

uint32_t sobel_golden_digest() { return kGoldenDigest; }

uint32_t sobel_total_pixels() { return kOutputWidth * kOutputHeight; }

}  // namespace bisen
