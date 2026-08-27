#ifndef BISEN_CHECKPOINT_H
#define BISEN_CHECKPOINT_H

#include "bisen_config.h"

namespace bisen {

enum class CheckpointStatus : uint8_t {
    kRestored = 0,
    kSaved,
    kNotFound,
    kDisabled,
    kStorageLayoutInvalid,
    kHalError,
    kVerificationFailed,
};

struct CheckpointInfo {
    uint32_t slot_index;
    uint32_t sequence;
    uint32_t interrupted_slot_index;
    uint32_t interrupted_sequence;
};

struct CheckpointLayout {
    uintptr_t begin;
    uintptr_t end;
    uint32_t slot_bytes;
};

CheckpointStatus restore_checkpoint(Context *context, CheckpointInfo *info = nullptr);
CheckpointStatus save_checkpoint(const Context &context, CheckpointInfo *info = nullptr);
CheckpointLayout checkpoint_layout();
bool checkpoint_layout_is_valid();

}  // namespace bisen

#endif  // BISEN_CHECKPOINT_H
