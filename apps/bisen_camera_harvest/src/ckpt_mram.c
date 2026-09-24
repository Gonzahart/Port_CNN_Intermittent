// App-local two-slot storage backend for the camera workload checkpoint.
// The linker, not a guessed address, supplies the reserved MRAM window.
#include <stdint.h>
#include <string.h>

#include "am_mcu_apollo.h"
#include "am_util.h"
#include "log_control.h"
#include "ckpt.h"

#ifndef CKPT_MRAM_TRACE
#define CKPT_MRAM_TRACE 1
#endif

#ifndef CKPT_USE_MRAM
#define CKPT_USE_MRAM 1
#endif

uint32_t g_ckpt_dev_programs;
uint32_t g_ckpt_dev_bytes;
uint32_t g_ckpt_dev_hal_program_calls;
uint32_t g_ckpt_dev_hal_program_successes;
uint32_t g_ckpt_dev_program_units;

#if CKPT_USE_MRAM

extern uint8_t __bisen_camera_checkpoint_start__[];
extern uint8_t __bisen_camera_checkpoint_end__[];
extern uint8_t __bisen_camera_session_start__[];
extern uint8_t __bisen_camera_session_end__[];

typedef char ckpt_linker_size_matches[
    (CKPT_NUM_SLOTS * CKPT_SLOT_SZ == 16160u) ? 1 : -1];

static uint32_t s_bounce[16];  // 64-byte, word-aligned source buffer

static int layout_valid(void) {
    const uintptr_t begin = (uintptr_t)__bisen_camera_checkpoint_start__;
    const uintptr_t end = (uintptr_t)__bisen_camera_checkpoint_end__;
    const uintptr_t mram_begin = (uintptr_t)AM_HAL_MRAM_ADDR;
    const uintptr_t mram_end = mram_begin + (uintptr_t)AM_HAL_MRAM_TOTAL_SIZE;
    return begin % CKPT_ALIGN == 0u && end >= begin &&
           end - begin == CKPT_NUM_SLOTS * CKPT_SLOT_SZ &&
           begin >= mram_begin && end <= mram_end;
}

static int session_layout_valid(void) {
    const uintptr_t begin = (uintptr_t)__bisen_camera_session_start__;
    const uintptr_t end = (uintptr_t)__bisen_camera_session_end__;
    const uintptr_t mram_begin = (uintptr_t)AM_HAL_MRAM_ADDR;
    const uintptr_t mram_end = mram_begin + (uintptr_t)AM_HAL_MRAM_TOTAL_SIZE;
    return begin % CKPT_ALIGN == 0u && end >= begin &&
           end - begin ==
               CKPT_SESSION_NUM_SLOTS * CKPT_SESSION_RECORD_SZ &&
           begin >= mram_begin && end <= mram_end;
}

static uintptr_t slot_addr(uint32_t slot, uint32_t off) {
    return (uintptr_t)__bisen_camera_checkpoint_start__ +
           (uintptr_t)slot * CKPT_SLOT_SZ + off;
}

static int program_words(uint32_t *src, uintptr_t dst, uint32_t words) {
    g_ckpt_dev_hal_program_calls++;
    g_ckpt_dev_program_units += words / (CKPT_ALIGN / sizeof(uint32_t));
    const uint32_t irq_state = am_hal_interrupt_master_disable();
    const int status = am_hal_mram_main_program(
        AM_HAL_MRAM_PROGRAM_KEY, src, (uint32_t *)dst, words);
    am_hal_interrupt_master_set(irq_state);
    if (status == AM_HAL_STATUS_SUCCESS) {
        g_ckpt_dev_hal_program_successes++;
    }
    return status;
}

void ckpt_dev_init(void) {
    if (!layout_valid() || !session_layout_valid()) {
        am_util_stdio_printf("BISen camera checkpoint layout INVALID\n");
    }
}

int ckpt_dev_write(uint32_t slot, uint32_t off, const void *src, uint32_t len) {
    if (!layout_valid() || src == 0 || slot >= CKPT_NUM_SLOTS ||
        off % CKPT_ALIGN != 0u || len == 0u || len % CKPT_ALIGN != 0u ||
        off > CKPT_SLOT_SZ || len > CKPT_SLOT_SZ - off) {
        return -1;
    }

    uintptr_t dst = slot_addr(slot, off);
    const uintptr_t region_end =
        (uintptr_t)__bisen_camera_checkpoint_end__;
    if (dst < (uintptr_t)__bisen_camera_checkpoint_start__ ||
        dst + len < dst || dst + len > region_end) {
        return -1;
    }

    g_ckpt_dev_programs++;
    g_ckpt_dev_bytes += len;
#if CKPT_MRAM_TRACE
    am_util_stdio_printf(
        "BISen camera MRAM program #%u: slot=%u off=%u bytes=%u addr=0x%08x\n",
        (unsigned)g_ckpt_dev_programs, (unsigned)slot, (unsigned)off,
        (unsigned)len, (unsigned)dst);
#endif

    const uint8_t *cursor = (const uint8_t *)src;
    uint32_t remaining = len;
    while (remaining != 0u) {
        const uint32_t chunk =
            remaining > sizeof(s_bounce) ? (uint32_t)sizeof(s_bounce)
                                          : remaining;
        memcpy(s_bounce, cursor, chunk);
        if (program_words(s_bounce, dst, chunk / sizeof(uint32_t)) !=
            AM_HAL_STATUS_SUCCESS) {
            return -1;
        }
        cursor += chunk;
        dst += chunk;
        remaining -= chunk;
    }
    return 0;
}

int ckpt_dev_read(uint32_t slot, uint32_t off, void *dst, uint32_t len) {
    if (!layout_valid() || dst == 0 || slot >= CKPT_NUM_SLOTS ||
        off > CKPT_SLOT_SZ || len > CKPT_SLOT_SZ - off) {
        return -1;
    }
    (void)am_hal_daxi_control(AM_HAL_DAXI_CONTROL_FLUSH, 0);
    (void)am_hal_cachectrl_control(
        AM_HAL_CACHECTRL_CONTROL_MRAM_CACHE_INVALIDATE, 0);
    memcpy(dst, (const void *)slot_addr(slot, off), len);
    return 0;
}

int ckpt_dev_session_write(uint32_t slot, const void *src) {
    if (!session_layout_valid() || src == 0 ||
        slot >= CKPT_SESSION_NUM_SLOTS) {
        return -1;
    }
    const uintptr_t dst = (uintptr_t)__bisen_camera_session_start__ +
                          slot * CKPT_SESSION_RECORD_SZ;
    g_ckpt_dev_programs++;
    g_ckpt_dev_bytes += CKPT_SESSION_RECORD_SZ;
#if CKPT_MRAM_TRACE
    am_util_stdio_printf(
        "BISen camera MRAM session program #%u: slot=%u bytes=%u addr=0x%08x\n",
        (unsigned)g_ckpt_dev_programs, (unsigned)slot,
        (unsigned)CKPT_SESSION_RECORD_SZ, (unsigned)dst);
#endif
    memcpy(s_bounce, src, CKPT_SESSION_RECORD_SZ);
    return program_words(s_bounce, dst,
                         CKPT_SESSION_RECORD_SZ / sizeof(uint32_t)) ==
                   AM_HAL_STATUS_SUCCESS
               ? 0
               : -1;
}

int ckpt_dev_session_read(uint32_t slot, void *dst) {
    if (!session_layout_valid() || dst == 0 ||
        slot >= CKPT_SESSION_NUM_SLOTS) {
        return -1;
    }
    (void)am_hal_daxi_control(AM_HAL_DAXI_CONTROL_FLUSH, 0);
    (void)am_hal_cachectrl_control(
        AM_HAL_CACHECTRL_CONTROL_MRAM_CACHE_INVALIDATE, 0);
    memcpy(dst,
           (const void *)((uintptr_t)__bisen_camera_session_start__ +
                          slot * CKPT_SESSION_RECORD_SZ),
           CKPT_SESSION_RECORD_SZ);
    return 0;
}

#else

// MRAM-off validation backend. It survives warm resets only and cannot touch
// application MRAM, which makes it useful for first camera-hardware bring-up.
#define NS_PERSIST __attribute__((section(".persist")))
NS_PERSIST static uint8_t s_slots[CKPT_NUM_SLOTS][CKPT_SLOT_SZ];
NS_PERSIST static uint8_t
    s_session_slots[CKPT_SESSION_NUM_SLOTS][CKPT_SESSION_RECORD_SZ];

void ckpt_dev_init(void) {}

int ckpt_dev_write(uint32_t slot, uint32_t off, const void *src, uint32_t len) {
    if (src == 0 || slot >= CKPT_NUM_SLOTS || off % CKPT_ALIGN != 0u ||
        len == 0u || len % CKPT_ALIGN != 0u || off > CKPT_SLOT_SZ ||
        len > CKPT_SLOT_SZ - off) {
        return -1;
    }
    memcpy(&s_slots[slot][off], src, len);
    __asm volatile("dsb 0xF" ::: "memory");
    am_hal_sysctrl_bus_write_flush();
    return 0;
}

int ckpt_dev_read(uint32_t slot, uint32_t off, void *dst, uint32_t len) {
    if (dst == 0 || slot >= CKPT_NUM_SLOTS || off > CKPT_SLOT_SZ ||
        len > CKPT_SLOT_SZ - off) {
        return -1;
    }
    memcpy(dst, &s_slots[slot][off], len);
    return 0;
}

int ckpt_dev_session_write(uint32_t slot, const void *src) {
    if (src == 0 || slot >= CKPT_SESSION_NUM_SLOTS) return -1;
    memcpy(s_session_slots[slot], src, CKPT_SESSION_RECORD_SZ);
    __asm volatile("dsb 0xF" ::: "memory");
    am_hal_sysctrl_bus_write_flush();
    return 0;
}

int ckpt_dev_session_read(uint32_t slot, void *dst) {
    if (dst == 0 || slot >= CKPT_SESSION_NUM_SLOTS) return -1;
    memcpy(dst, s_session_slots[slot], CKPT_SESSION_RECORD_SZ);
    return 0;
}

#endif
