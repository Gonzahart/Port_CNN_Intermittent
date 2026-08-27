#ifndef BISEN_LOG_H
#define BISEN_LOG_H

#include "bisen_runtime.h"

namespace bisen {

enum class BisenLogEvent : uint16_t {
    kBoot = 0,
    kAdc,
    kPolicy,
    kTemperature,
    kCompute,
    kCheckpointAttempt,
    kCheckpointCommitted,
    kSleep,
    kWake,
    kRunMode,
    kError,
};

struct BisenLogEntry {
    uint32_t sequence;
    uint32_t wake_count;
    uint32_t vcap_mv;
    int32_t temperature_mc;
    uint32_t job_id;
    uint32_t progress;
    uint32_t digest;
    uint16_t event;
    uint16_t flags;
};

static_assert(sizeof(BisenLogEntry) == 32u,
              "RAM event-log entry must remain exactly 32 bytes");

void bisen_log_reset();
void bisen_log_append(BisenLogEvent event, uint16_t flags,
                      uint32_t wake_count,
                      const BisenRuntimeContext &runtime);
uint32_t bisen_log_count();
uint32_t bisen_log_capacity();
uint32_t bisen_log_wrap_count();
uint32_t bisen_log_storage_bytes();
bool bisen_log_read_oldest(uint32_t offset, BisenLogEntry *entry);

}  // namespace bisen

#endif  // BISEN_LOG_H
