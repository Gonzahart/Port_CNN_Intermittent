#include "bisen_checkpoint.h"

#include <stddef.h>

#include "am_mcu_apollo.h"
#include "bisen_compute.h"

extern "C" uint8_t __bisen_checkpoint_start__[];
extern "C" uint8_t __bisen_checkpoint_end__[];

namespace bisen {
namespace {

constexpr uint32_t kCheckpointMagic = 0x4249534eu;  // "BISN"
#if BISEN_ENABLE_RESET_INJECTION
// Give each destructive reset-test phase its own record epoch. Reflashing a
// different phase therefore cannot accidentally reuse the previous phase's
// committed or interrupted test records, even if the linker layout is equal.
constexpr uint32_t kCheckpointVersion =
    0x110u + static_cast<uint32_t>(BISEN_RESET_INJECTION_PHASE);
#elif BISEN_ENABLE_COMPUTE_RESUME_TEST
// Do not accept state left by the checkpoint-only tests as workload progress.
constexpr uint32_t kCheckpointVersion = 0x210u;
#elif BISEN_ENABLE_INTEGRATED_CYCLE_TEST
// Keep autonomous sleep/checkpoint/compute validation isolated from every
// manually stepped checkpoint epoch. Version 0x322 identifies the main.c-
// aligned pre-sleep, dirty-incomplete-progress production scheduler.
constexpr uint32_t kCheckpointVersion = 0x322u;
#else
constexpr uint32_t kCheckpointVersion = 2u;
#endif
constexpr uint32_t kCommitted = 0x434f4d4du;        // "COMM"
constexpr uint32_t kUncommitted = 0xffffffffu;

struct alignas(16) CheckpointIntegrity {
    uint32_t payload_crc32;
    uint32_t record_bytes;
    uint32_t reserved;
    uint32_t commit;
};

struct alignas(16) CheckpointRecord {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t job_id;
    uint32_t completion_mask;
    uint32_t compute_index;
    uint32_t output_progress;
    uint32_t workload_digest;
    int32_t temperature_millicelsius;
    uint32_t vcap_millivolts;
    uint32_t resume_state;
    uint32_t energy_band;
    uint32_t job_complete;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
    CheckpointIntegrity integrity;
};

static_assert((sizeof(CheckpointRecord) % 16u) == 0u,
              "MRAM record must be a multiple of one program block");
static_assert((offsetof(CheckpointRecord, integrity) % 16u) == 0u,
              "MRAM commit must occupy a final 16-byte block");

#if BISEN_ENABLE_MRAM_CHECKPOINTS
uint32_t crc32(const void *data, uint32_t bytes) {
    const uint8_t *cursor = static_cast<const uint8_t *>(data);
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < bytes; ++i) {
        crc ^= cursor[i];
        for (uint32_t bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

volatile const CheckpointRecord *slot(uint32_t index) {
    return reinterpret_cast<volatile const CheckpointRecord *>(
        __bisen_checkpoint_start__ + index * sizeof(CheckpointRecord));
}

bool payload_is_valid(const CheckpointRecord &record) {
    constexpr uint32_t kKnownCompletionBits =
        static_cast<uint32_t>(kTemperatureComplete) |
        static_cast<uint32_t>(kComputeComplete);
    Context context = {};
    context.completion_mask = record.completion_mask;
    context.compute_index = record.compute_index;
    context.output_progress = record.output_progress;
    context.workload_digest = record.workload_digest;
    context.job_complete = record.job_complete != 0u;
    return record.magic == kCheckpointMagic &&
           record.version == kCheckpointVersion &&
           record.integrity.record_bytes == sizeof(CheckpointRecord) &&
           (record.completion_mask & ~kKnownCompletionBits) == 0u &&
           record.resume_state <= static_cast<uint32_t>(State::kSleepLowPowerWait) &&
           record.energy_band <= static_cast<uint32_t>(EnergyBand::kChunk1000) &&
           record.job_complete <= 1u &&
           sobel_context_is_valid(context) &&
           record.integrity.payload_crc32 ==
               crc32(&record, offsetof(CheckpointRecord, integrity));
}

bool record_is_valid(const CheckpointRecord &record) {
    return record.integrity.commit == kCommitted && payload_is_valid(record);
}

CheckpointRecord read_slot(uint32_t index) {
    (void)am_hal_daxi_control(AM_HAL_DAXI_CONTROL_FLUSH, nullptr);
    (void)am_hal_cachectrl_control(AM_HAL_CACHECTRL_CONTROL_MRAM_CACHE_INVALIDATE,
                                   nullptr);
    const volatile CheckpointRecord *source = slot(index);
    CheckpointRecord copy = {};
    const volatile uint32_t *from = reinterpret_cast<const volatile uint32_t *>(source);
    uint32_t *to = reinterpret_cast<uint32_t *>(&copy);
    for (uint32_t i = 0; i < sizeof(copy) / sizeof(uint32_t); ++i) {
        to[i] = from[i];
    }
    return copy;
}

CheckpointRecord make_record(const Context &context, uint32_t sequence) {
    CheckpointRecord record = {};
    record.magic = kCheckpointMagic;
    record.version = kCheckpointVersion;
    record.sequence = sequence;
    record.job_id = context.job_id;
    record.completion_mask = context.completion_mask;
    record.compute_index = context.compute_index;
    record.output_progress = context.output_progress;
    record.workload_digest = context.workload_digest;
    record.temperature_millicelsius = context.temperature_millicelsius;
    record.vcap_millivolts = context.vcap_millivolts;
    record.resume_state = static_cast<uint32_t>(context.resume_state);
    record.energy_band = static_cast<uint32_t>(context.energy_band);
    record.job_complete = context.job_complete ? 1u : 0u;
    record.integrity.record_bytes = sizeof(record);
    record.integrity.commit = kUncommitted;
    record.integrity.payload_crc32 = crc32(&record, offsetof(CheckpointRecord, integrity));
    return record;
}

bool sequence_is_newer(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
}

void restore_context(const CheckpointRecord &record, Context *context) {
    context->job_id = record.job_id;
    context->completion_mask = record.completion_mask;
    context->compute_index = record.compute_index;
    context->output_progress = record.output_progress;
    context->workload_digest = record.workload_digest;
    context->temperature_millicelsius = record.temperature_millicelsius;
    context->vcap_millivolts = record.vcap_millivolts;
    context->resume_state = static_cast<State>(record.resume_state);
    context->energy_band = static_cast<EnergyBand>(record.energy_band);
    context->job_complete = record.job_complete != 0u;
}

void maybe_inject_reset(uint32_t phase, bool armed) {
#if BISEN_ENABLE_RESET_INJECTION
    if (armed && BISEN_RESET_INJECTION_PHASE == phase) {
        NVIC_SystemReset();
    }
#else
    (void)phase;
    (void)armed;
#endif
}

int program_mram_blocks(uint32_t *source, uint32_t *destination,
                        uint32_t word_count) {
    // AmbiqSuite's Apollo4 Cordio persistence examples protect each direct
    // MRAM program operation with this exact interrupt save/restore sequence.
    const uint32_t interrupt_state = am_hal_interrupt_master_disable();
    const int status = am_hal_mram_main_program(
        AM_HAL_MRAM_PROGRAM_KEY, source, destination, word_count);
    am_hal_interrupt_master_set(interrupt_state);
    return status;
}
#endif  // BISEN_ENABLE_MRAM_CHECKPOINTS

}  // namespace

CheckpointLayout checkpoint_layout() {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(__bisen_checkpoint_start__);
    const uintptr_t end = reinterpret_cast<uintptr_t>(__bisen_checkpoint_end__);
    return {begin, end, sizeof(CheckpointRecord)};
}

bool checkpoint_layout_is_valid() {
    const CheckpointLayout layout = checkpoint_layout();
    const uintptr_t mram_begin = static_cast<uintptr_t>(AM_HAL_MRAM_ADDR);
    const uintptr_t mram_end = mram_begin +
                               static_cast<uintptr_t>(AM_HAL_MRAM_TOTAL_SIZE);
    return layout.end >= layout.begin &&
           (layout.begin % 16u) == 0u &&
           (layout.slot_bytes % 16u) == 0u &&
           (layout.end - layout.begin) == 2u * layout.slot_bytes &&
           layout.begin >= mram_begin && layout.end <= mram_end;
}

CheckpointStatus restore_checkpoint(Context *context, CheckpointInfo *info) {
    if (info != nullptr) {
        info->slot_index = UINT32_MAX;
        info->sequence = 0u;
        info->interrupted_slot_index = UINT32_MAX;
        info->interrupted_sequence = 0u;
    }
#if !BISEN_ENABLE_MRAM_CHECKPOINTS
    (void)context;
    return CheckpointStatus::kDisabled;
#else
    if (context == nullptr) {
        return CheckpointStatus::kHalError;
    }
    if (!checkpoint_layout_is_valid()) {
        return CheckpointStatus::kStorageLayoutInvalid;
    }
    const CheckpointRecord first = read_slot(0u);
    const CheckpointRecord second = read_slot(1u);
    const bool first_valid = record_is_valid(first);
    const bool second_valid = record_is_valid(second);
    if (!first_valid && !second_valid) {
        return CheckpointStatus::kNotFound;
    }
    const uint32_t selected =
        !second_valid || (first_valid && sequence_is_newer(first.sequence, second.sequence))
            ? 0u
            : 1u;
    const CheckpointRecord &newest = selected == 0u ? first : second;
    restore_context(newest, context);
    if (info != nullptr) {
        info->slot_index = selected;
        info->sequence = newest.sequence;
        const bool first_interrupted =
            first.integrity.commit == kUncommitted && payload_is_valid(first) &&
            sequence_is_newer(first.sequence, newest.sequence);
        const bool second_interrupted =
            second.integrity.commit == kUncommitted && payload_is_valid(second) &&
            sequence_is_newer(second.sequence, newest.sequence);
        if (first_interrupted || second_interrupted) {
            const bool choose_first =
                !second_interrupted ||
                (first_interrupted &&
                 sequence_is_newer(first.sequence, second.sequence));
            info->interrupted_slot_index = choose_first ? 0u : 1u;
            info->interrupted_sequence =
                choose_first ? first.sequence : second.sequence;
        }
    }
    return CheckpointStatus::kRestored;
#endif
}

CheckpointStatus save_checkpoint(const Context &context, CheckpointInfo *info) {
    if (info != nullptr) {
        info->slot_index = UINT32_MAX;
        info->sequence = 0u;
        info->interrupted_slot_index = UINT32_MAX;
        info->interrupted_sequence = 0u;
    }
#if !BISEN_ENABLE_MRAM_CHECKPOINTS
    (void)context;
    return CheckpointStatus::kDisabled;
#else
    if (!checkpoint_layout_is_valid()) {
        return CheckpointStatus::kStorageLayoutInvalid;
    }
    const CheckpointRecord first = read_slot(0u);
    const CheckpointRecord second = read_slot(1u);
    const bool first_valid = record_is_valid(first);
    const bool second_valid = record_is_valid(second);
    const uint32_t newest_sequence =
        !first_valid ? (second_valid ? second.sequence : 0u)
                     : (!second_valid || sequence_is_newer(first.sequence, second.sequence)
                            ? first.sequence
                            : second.sequence);
    const uint32_t target = !first_valid ? 0u : (!second_valid ? 1u :
        (sequence_is_newer(first.sequence, second.sequence) ? 1u : 0u));
    CheckpointRecord record = make_record(context, newest_sequence + 1u);
    // The controlled reset test is armed only for the first update after a
    // normally committed seed record. This prevents a reset loop.
    const bool reset_injection_armed = newest_sequence == 1u;
    maybe_inject_reset(1u, reset_injection_armed);
    if (program_mram_blocks(
            reinterpret_cast<uint32_t *>(&record),
            reinterpret_cast<uint32_t *>(
                const_cast<CheckpointRecord *>(slot(target))),
            sizeof(record) / sizeof(uint32_t)) != AM_HAL_STATUS_SUCCESS) {
        return CheckpointStatus::kHalError;
    }
    maybe_inject_reset(2u, reset_injection_armed);
    const CheckpointRecord written = read_slot(target);
    if (!payload_is_valid(written) || written.integrity.commit != kUncommitted) {
        return CheckpointStatus::kVerificationFailed;
    }
    CheckpointIntegrity final_block = record.integrity;
    final_block.commit = kCommitted;
    if (program_mram_blocks(
            reinterpret_cast<uint32_t *>(&final_block),
            reinterpret_cast<uint32_t *>(const_cast<CheckpointIntegrity *>(
                &slot(target)->integrity)),
            sizeof(final_block) / sizeof(uint32_t)) != AM_HAL_STATUS_SUCCESS) {
        return CheckpointStatus::kHalError;
    }
    maybe_inject_reset(3u, reset_injection_armed);
    const CheckpointRecord committed = read_slot(target);
    if (!record_is_valid(committed)) {
        return CheckpointStatus::kVerificationFailed;
    }
    if (info != nullptr) {
        info->slot_index = target;
        info->sequence = committed.sequence;
    }
    return CheckpointStatus::kSaved;
#endif
}

}  // namespace bisen
