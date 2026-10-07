/*
 * dsplib_sw.c -- TI MSP DSPLib routines used by Capuchin, generic C versions.
 *
 * Copied from TI MSP DSPLib as vendored in Capuchin 76b6eb2
 * (capuchin-MCU/DSPLib/source/...), taking the branch DSPLib compiles on a
 * device WITHOUT the LEA and WITHOUT the MPY32 peripheral -- i.e. TI's own
 * portable reference. Function bodies are unchanged apart from the optional
 * CAPUCHIN_NUMERIC_AUDIT counters, which compile to nothing on the board.
 *
 *   Copyright (c) 2016, Texas Instruments Incorporated. All rights reserved.
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted under the BSD-3-Clause terms reproduced in
 *   capuchin-MCU/DSPLib of the upstream repository.
 *
 * Arithmetic of msp_matrix_mpy_q15 (generic C): 32-bit accumulation of q15
 * products, then (acc >> 15) saturated to int16. Whether this is bit-identical
 * to the LEA's MPYMATRIXROW on the MSP430FR5994 is checked empirically against
 * the native MSP430 run (gate X1 in docs/RUIC_V8_Capuchin_Apollo4_Port.md).
 *
 * Build with -fwrapv: the TI code relies on two's-complement wrap of int32.
 */
#include "DSPLib.h"

volatile uint32_t g_capuchin_port_error = 0;

#ifdef CAPUCHIN_NUMERIC_AUDIT
#include "capuchin_audit.h"
capuchin_audit_t g_capuchin_audit;
#endif

#ifndef CAPUCHIN_KERNEL
#define CAPUCHIN_KERNEL 1
#endif

#if CAPUCHIN_KERNEL == 1
/* DSPLib/source/matrix/msp_matrix_mpy_q15.c, "#else //__MSP430_HAS_MPY32__" branch */
msp_status msp_matrix_mpy_q15(const msp_matrix_mpy_q15_params *params, const _q15 *srcA, const _q15 *srcB, _q15 *dst)
{
    uint16_t cntr;
    uint16_t srcARows;
    uint16_t srcACols;
    uint16_t srcBRows;
    uint16_t srcBCols;
    uint16_t dst_row;
    uint16_t dst_col;
    uint16_t row_offset;
    uint16_t col_offset;
    uint16_t dst_row_offset;

    /* Initialize the row and column sizes. */
    srcARows = params->srcARows;
    srcACols = params->srcACols;
    srcBRows = params->srcBRows;
    srcBCols = params->srcBCols;

#ifndef MSP_DISABLE_DIAGNOSTICS
    /* Check that column of A equals rows of B */
    if (srcACols != srcBRows) {
        return MSP_SIZE_ERROR;
    }
#endif //MSP_DISABLE_DIAGNOSTICS

    /* In initialize loop counters. */
    cntr = 0;
    dst_row = 0;
    dst_col = 0;
    row_offset = 0;
    col_offset = 0;
    dst_row_offset = 0;

    _iq31 result;
#ifdef CAPUCHIN_NUMERIC_AUDIT
    int64_t shadow;
#endif

    /* Loop through all srcA rows. */
    while(srcARows--) {
        /* Loop through all srcB columns. */
        while (dst_col < srcBCols) {
            /* Initialize accumulator. */
            result = 0;
#ifdef CAPUCHIN_NUMERIC_AUDIT
            shadow = 0;
#endif

            /* Loop through all elements in srcA column and srcB row. */
            while(cntr < srcACols) {
                result += (_iq31)srcA[row_offset + cntr] * (_iq31)srcB[col_offset + dst_col];
#ifdef CAPUCHIN_NUMERIC_AUDIT
                shadow += (int64_t)srcA[row_offset + cntr] * (int64_t)srcB[col_offset + dst_col];
#endif
                col_offset += srcBCols;
                cntr++;
            }

#ifdef CAPUCHIN_NUMERIC_AUDIT
            /* Column 0 is the only column Capuchin reads back (LEA_RESERVED). */
            if (dst_col == 0) {
                g_capuchin_audit.mpy_outputs++;
                if (shadow != (int64_t)result) g_capuchin_audit.mpy_acc32_overflow++;
                if ((result >> 15) > INT16_MAX || (result >> 15) < INT16_MIN) g_capuchin_audit.mpy_out_saturated++;
            }
#endif
            /* Saturate and store the result */
            dst[dst_row_offset + dst_col] = (_q15)__saturate(result >> 15, INT16_MIN, INT16_MAX);

            /* Update pointers. */
            dst_col++;
            cntr = 0;
            col_offset = 0;
        }

        /* Update pointers. */
        dst_row++;
        dst_col = 0;
        row_offset += srcACols;
        dst_row_offset += srcBCols;
    }

    return MSP_SUCCESS;
}
#endif /* CAPUCHIN_KERNEL == 1 */

/* DSPLib/source/matrix/msp_matrix_shift_q15.c (unchanged) */
msp_status msp_matrix_shift_q15(const msp_matrix_shift_q15_params *params, const _q15 *src, _q15 *dst)
{
    msp_shift_q15_params paramsTemp;

    /* Use real vector shift function. */
    paramsTemp.shift = params->shift;
    paramsTemp.length = params->rows * params->cols;

    return msp_shift_q15(&paramsTemp, src, dst);
}

/* DSPLib/source/vector/msp_shift_q15.c. The left-shift loop is plain C in TI's
 * file for LEA and non-LEA devices alike; the right shift is the non-LEA helper. */
static inline msp_status msp_shift_right_q15(const _q15 *src, _q15 *dst, uint16_t length, uint8_t shift)
{
    /* Loop through all vector elements. */
    while (length--) {
        /* Shift src right and store to dst. */
        *dst++ = *src++ >> shift;
    }

    return MSP_SUCCESS;
}

msp_status msp_shift_q15(const msp_shift_q15_params *params, const _q15 *src, _q15 *dst)
{
    int8_t shift;               // Shift count
    uint16_t length;            // Shift length

    /* Initialize the loop counter and shift variables. */
    length = params->length;
    shift = params->shift;

#ifndef MSP_DISABLE_DIAGNOSTICS
    /* Verify the shift parameter. */
    if ((shift > 15) || (shift < -15)) {
        return MSP_SHIFT_SIZE_ERROR;
    }
#endif //MSP_DISABLE_DIAGNOSTICS

    /* Shift src array left for a positive shift parameter. */
    if (shift > 0) {
        /* Loop through all vector elements. */
        while (length--) {
#ifdef CAPUCHIN_NUMERIC_AUDIT
            /* Even positions = LEA_RESERVED column 0, the one Capuchin reads. */
            if ((length & 1u) == ((params->length - 1u) & 1u)) {
                int32_t wide = (int32_t)*src * (1 << shift);
                g_capuchin_audit.shift_outputs++;
                if (wide != (int16_t)wide) g_capuchin_audit.shift_wrapped++;
            }
#endif
            /* Shift src left by the shift parameter and store to dst. */
            *dst++ = *src++ << shift;
        }
    }
    /* Shift src array right for a negative shift parameter. */
    else {
        /* Use optimized helper function. */
        return msp_shift_right_q15(src, dst, length, -shift);
    }

    return MSP_SUCCESS;
}

/*
 * Referenced only by upstream filter_LEA(), which no executed path calls
 * (its call is commented out in layers.c filters_sum at 76b6eb2). If that ever
 * changes, fail loudly instead of computing something unverified.
 */
msp_status msp_mac_q15(const msp_mac_q15_params *params, const _q15 *srcA,
                       const _q15 *srcB, _iq31 *result)
{
    (void)params; (void)srcA; (void)srcB; (void)result;
    return MSP_LEA_INCORRECT_REVISION;
}
msp_status msp_shift_iq31(const msp_shift_iq31_params *params, const _iq31 *src, _iq31 *dst)
{
    (void)params; (void)src; (void)dst;
    return MSP_LEA_INCORRECT_REVISION;
}
msp_status msp_iq31_to_q15(const msp_iq31_to_q15_params *params, const _iq31 *src, _q15 *dst)
{
    (void)params; (void)src; (void)dst;
    return MSP_LEA_INCORRECT_REVISION;
}
