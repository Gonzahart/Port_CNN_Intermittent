/* capuchin_audit.h -- host-only numeric audit counters (CAPUCHIN_NUMERIC_AUDIT).
 * Never enabled in board builds; the counters do not change any result. */
#ifndef CAPUCHIN_AUDIT_H
#define CAPUCHIN_AUDIT_H
#include <stdint.h>
typedef struct {
    uint64_t mpy_outputs;         /* q15 matrix-multiply outputs read back (column 0) */
    uint64_t mpy_acc32_overflow;  /* 32-bit accumulator differed from exact 64-bit sum */
    uint64_t mpy_out_saturated;   /* (acc >> 15) clipped to int16 */
    uint64_t shift_outputs;       /* left-shifted q15 values read back */
    uint64_t shift_wrapped;       /* left shift lost high bits (int16 wrap) */
} capuchin_audit_t;
extern capuchin_audit_t g_capuchin_audit;
#endif
