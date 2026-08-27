#include "bisen_log.h"

namespace {

constexpr uint32_t kLogCapacity = 128u;

#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
struct BisenLogRing {
    bisen::BisenLogEntry entries[kLogCapacity];
    uint32_t next_index;
    uint32_t count;
    uint32_t next_sequence;
    uint32_t wrap_count;
};

static_assert(sizeof(BisenLogRing) == 4112u,
              "RAM event-log allocation changed; update map evidence");

#if defined(BISEN_HOST_TEST)
__attribute__((aligned(8)))
#else
__attribute__((section(".bisen_retained"), aligned(8)))
#endif
BisenLogRing g_bisen_log;
#endif

}  // namespace

namespace bisen {

void bisen_log_reset() {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    g_bisen_log = {};
#endif
}

void bisen_log_append(BisenLogEvent event, uint16_t flags,
                      uint32_t wake_count,
                      const BisenRuntimeContext &runtime) {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    BisenLogEntry &entry = g_bisen_log.entries[g_bisen_log.next_index];
    entry.sequence = ++g_bisen_log.next_sequence;
    entry.wake_count = wake_count;
    entry.vcap_mv = runtime.context.vcap_millivolts;
    entry.temperature_mc = runtime.context.temperature_millicelsius;
    entry.job_id = runtime.context.job_id;
    entry.progress = runtime.context.compute_index;
    entry.digest = runtime.context.workload_digest;
    entry.event = static_cast<uint16_t>(event);
    entry.flags = flags;
    g_bisen_log.next_index =
        (g_bisen_log.next_index + 1u) % kLogCapacity;
    if (g_bisen_log.count < kLogCapacity) {
        ++g_bisen_log.count;
    } else {
        ++g_bisen_log.wrap_count;
    }
#else
    (void)event;
    (void)flags;
    (void)wake_count;
    (void)runtime;
#endif
}

uint32_t bisen_log_count() {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    return g_bisen_log.count;
#else
    return 0u;
#endif
}

uint32_t bisen_log_capacity() {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    return kLogCapacity;
#else
    return 0u;
#endif
}

uint32_t bisen_log_wrap_count() {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    return g_bisen_log.wrap_count;
#else
    return 0u;
#endif
}

uint32_t bisen_log_storage_bytes() {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    return sizeof(g_bisen_log);
#else
    return 0u;
#endif
}

bool bisen_log_read_oldest(uint32_t offset, BisenLogEntry *entry) {
#if BISEN_LOG_BACKEND == BISEN_LOG_BACKEND_RAM
    if (entry == nullptr || offset >= g_bisen_log.count) {
        return false;
    }
    const uint32_t oldest =
        (g_bisen_log.next_index + kLogCapacity - g_bisen_log.count) %
        kLogCapacity;
    *entry = g_bisen_log.entries[(oldest + offset) % kLogCapacity];
    return true;
#else
    (void)offset;
    (void)entry;
    return false;
#endif
}

}  // namespace bisen
