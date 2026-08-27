#include <assert.h>
#include <stdio.h>

#include "bisen_log.h"

int main() {
    bisen::runtime_context_reset();
    bisen::bisen_log_reset();
    bisen::BisenRuntimeContext *runtime = bisen::runtime_context();

    assert(bisen::bisen_log_capacity() == 128u);
    assert(bisen::bisen_log_storage_bytes() == 4112u);
    assert(bisen::bisen_log_count() == 0u);
    bisen::BisenLogEntry entry = {};
    assert(!bisen::bisen_log_read_oldest(0u, &entry));

    for (uint32_t i = 0u; i < 140u; ++i) {
        runtime->context.compute_index = i;
        runtime->context.output_progress = i;
        runtime->context.vcap_millivolts = 7000u + i;
        bisen::bisen_log_append(bisen::BisenLogEvent::kCompute,
                                static_cast<uint16_t>(i), i, *runtime);
    }
    assert(bisen::bisen_log_count() == 128u);
    assert(bisen::bisen_log_wrap_count() == 12u);
    assert(bisen::bisen_log_read_oldest(0u, &entry));
    assert(entry.sequence == 13u);
    assert(entry.wake_count == 12u);
    assert(entry.progress == 12u);
    assert(entry.flags == 12u);
    assert(bisen::bisen_log_read_oldest(127u, &entry));
    assert(entry.sequence == 140u);
    assert(entry.wake_count == 139u);
    assert(entry.progress == 139u);
    assert(!bisen::bisen_log_read_oldest(128u, &entry));

    puts("bisen_log_host_test: PASS");
    return 0;
}
