#include <assert.h>
#include <stdint.h>

// Synthetic anchors exercise interpolation; firmware has no physical defaults.
#define BISEN_TRACE_CALIBRATION_MODE 0
#define BISEN_HARVEST_SWITCHED_DIVIDER 1
#define BISEN_HARVEST_CAL_LOW_CODE 1000
#define BISEN_HARVEST_CAL_LOW_UV 3000000
#define BISEN_HARVEST_CAL_HIGH_CODE 3000
#define BISEN_HARVEST_CAL_HIGH_UV 8000000
#define BISEN_HARVEST_CRITICAL_UV 5200000
#define BISEN_HARVEST_WORK100_UV 5600000
#define BISEN_HARVEST_WORK500_UV 6300000
#define BISEN_HARVEST_WORK1000_UV 7200000
#include "../src/trace_input.h"

int main(void) {
    assert(trace_input_microvolts(1000) == 3000000u);
    assert(trace_input_microvolts(3000) == 8000000u);
    assert(trace_policy_code(5199999u) < 2185u);
    assert(trace_policy_code(5200000u) == 2185u);
    assert(trace_policy_code(5599999u) < 2333u);
    assert(trace_policy_code(5600000u) == 2333u);
    assert(trace_policy_code(6299999u) < 2441u);
    assert(trace_policy_code(6300000u) == 2441u);
    assert(trace_policy_code(7199999u) < 2553u);
    assert(trace_policy_code(7200000u) == 2553u);
    assert(trace_policy_code(8000000u) == 2553u);
    uint32_t previous = 0u;
    for (uint32_t uv = 0u; uv <= 8000000u; uv += 1000u) {
        const uint32_t code = trace_policy_code(uv);
        assert(code >= previous);
        previous = code;
    }
    return 0;
}
