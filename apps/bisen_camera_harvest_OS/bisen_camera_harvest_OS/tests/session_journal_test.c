#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/ckpt.h"

uint32_t g_ckpt_dev_programs;
uint32_t g_ckpt_dev_bytes;
uint32_t g_ckpt_dev_hal_program_calls;
uint32_t g_ckpt_dev_hal_program_successes;
uint32_t g_ckpt_dev_program_units;

static uint8_t g_work_slots[CKPT_NUM_SLOTS][CKPT_SLOT_SZ];
static uint8_t
    g_session_slots[CKPT_SESSION_NUM_SLOTS][CKPT_SESSION_RECORD_SZ];
static int g_torn_session_write;

void ckpt_dev_init(void) {}

int ckpt_dev_write(uint32_t slot, uint32_t off, const void *src,
                   uint32_t len) {
    if (slot >= CKPT_NUM_SLOTS || off + len > CKPT_SLOT_SZ) return -1;
    memcpy(&g_work_slots[slot][off], src, len);
    return 0;
}

int ckpt_dev_read(uint32_t slot, uint32_t off, void *dst, uint32_t len) {
    if (slot >= CKPT_NUM_SLOTS || off + len > CKPT_SLOT_SZ) return -1;
    memcpy(dst, &g_work_slots[slot][off], len);
    return 0;
}

int ckpt_dev_session_write(uint32_t slot, const void *src) {
    if (slot >= CKPT_SESSION_NUM_SLOTS) return -1;
    if (g_torn_session_write) {
        memcpy(g_session_slots[slot], src, CKPT_SESSION_RECORD_SZ / 2u);
        return -1;
    }
    memcpy(g_session_slots[slot], src, CKPT_SESSION_RECORD_SZ);
    return 0;
}

int ckpt_dev_session_read(uint32_t slot, void *dst) {
    if (slot >= CKPT_SESSION_NUM_SLOTS) return -1;
    memcpy(dst, g_session_slots[slot], CKPT_SESSION_RECORD_SZ);
    return 0;
}

int main(void) {
    memset(g_work_slots, 0xff, sizeof(g_work_slots));
    memset(g_session_slots, 0xff, sizeof(g_session_slots));

    ckpt_init();
    assert(!ckpt_session_is_armed());
    assert(ckpt_session_set_armed(1) == 0);
    assert(ckpt_session_is_armed());

    // A reboot must recover the committed armed state.
    ckpt_init();
    assert(ckpt_session_is_armed());

    // A torn disarm may not supersede the previous valid armed record.
    g_torn_session_write = 1;
    assert(ckpt_session_set_armed(0) == -1);
    assert(ckpt_session_is_armed());
    ckpt_init();
    assert(ckpt_session_is_armed());

    // Retrying the disarm commits it to the alternate slot.
    g_torn_session_write = 0;
    assert(ckpt_session_set_armed(0) == 0);
    assert(!ckpt_session_is_armed());
    ckpt_init();
    assert(!ckpt_session_is_armed());
    return 0;
}
