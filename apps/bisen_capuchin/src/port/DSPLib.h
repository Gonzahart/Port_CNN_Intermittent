/*
 * DSPLib.h -- the subset of TI MSP DSPLib (v1.30 API, BSD-3-Clause, as shipped
 * in Capuchin's repository under capuchin-MCU/DSPLib) that Capuchin references.
 *
 * Types and parameter structures are copied from DSPLib_types.h,
 * DSPLib_matrix.h, DSPLib_vector.h and DSPLib_utility.h. Implementations live in
 * dsplib_sw.c (TI's generic C, non-LEA) and, for the matrix multiply only,
 * optionally in dsplib_cmsis.c (CMSIS-DSP on the Cortex-M4 DSP extension).
 *
 * On the MSP430FR5994 the only DSPLib routine Capuchin 76b6eb2 executes on the
 * LEA is msp_matrix_mpy_q15 (conv via filter_im2col, dense via
 * matrix_multiply). msp_matrix_shift_q15 is called with positive shifts only,
 * and DSPLib performs left shifts in plain C even on LEA devices.
 */
#ifndef CAPUCHIN_DSPLIB_STANDIN_H
#define CAPUCHIN_DSPLIB_STANDIN_H

#include <stdint.h>
#include <stdbool.h>
#include "capuchin_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int16_t _q15;
typedef int32_t _iq31;

typedef enum {
    MSP_SUCCESS,
    MSP_SIZE_ERROR,
    MSP_SHIFT_SIZE_ERROR,
    MSP_TABLE_SIZE_ERROR,
    MSP_LEA_BUSY,
    MSP_LEA_INVALID_ADDRESS,
    MSP_LEA_OUT_OF_RANGE,
    MSP_LEA_SCALAR_INCONSISTENCY,
    MSP_LEA_COMMAND_OVERFLOW,
    MSP_LEA_INCORRECT_REVISION
} msp_status;

typedef struct msp_matrix_mpy_q15_params {
    uint16_t srcARows;
    uint16_t srcACols;
    uint16_t srcBRows;
    uint16_t srcBCols;
} msp_matrix_mpy_q15_params;

typedef struct msp_matrix_shift_q15_params {
    uint16_t rows;
    uint16_t cols;
    int8_t shift;
} msp_matrix_shift_q15_params;

typedef struct msp_shift_q15_params {
    uint16_t length;
    int8_t shift;
} msp_shift_q15_params;

/* Used only by upstream filter_LEA(), whose call site is commented out
 * upstream (layers.c filters_sum). Present so that the file compiles. */
typedef struct msp_mac_q15_params { uint16_t length; } msp_mac_q15_params;
typedef struct msp_shift_iq31_params { uint16_t length; int8_t shift; } msp_shift_iq31_params;
typedef struct msp_iq31_to_q15_params { uint16_t length; } msp_iq31_to_q15_params;

msp_status msp_matrix_mpy_q15(const msp_matrix_mpy_q15_params *params,
                              const _q15 *srcA, const _q15 *srcB, _q15 *dst);
msp_status msp_matrix_shift_q15(const msp_matrix_shift_q15_params *params,
                                const _q15 *src, _q15 *dst);
msp_status msp_shift_q15(const msp_shift_q15_params *params, const _q15 *src, _q15 *dst);
msp_status msp_mac_q15(const msp_mac_q15_params *params, const _q15 *srcA,
                       const _q15 *srcB, _iq31 *result);
msp_status msp_shift_iq31(const msp_shift_iq31_params *params, const _iq31 *src, _iq31 *dst);
msp_status msp_iq31_to_q15(const msp_iq31_to_q15_params *params, const _iq31 *src, _q15 *dst);

/* DSPLib_support.h */
#define __saturate(x, min, max) (((x)>(max))?(max):(((x)<(min))?(min):(x)))

/* TI: DSPLIB_DATA(var, align) places var in LEA RAM with alignment. Apollo4 has
 * no LEA RAM; the buffer is ordinary SRAM. Upstream writes "DSPLIB_DATA(x,4);". */
#define DSPLIB_DATA(var, align)

/* TI loops forever on any error status. Same here, but the cause is recorded
 * first so a debugger/bench log can identify it. */
static inline void msp_checkStatus(msp_status status)
{
    if (status != MSP_SUCCESS) {
        g_capuchin_port_error = 0x44530000u | (uint32_t)status;  /* 'DS' */
        while (true) { }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* CAPUCHIN_DSPLIB_STANDIN_H */
