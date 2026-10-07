/*
 * dsplib_cmsis.c -- CAPUCHIN_KERNEL=2: serve Capuchin's one LEA operation,
 * msp_matrix_mpy_q15, with CMSIS-DSP arm_mat_mult_q15 on the Cortex-M4 DSP
 * extension (dual 16x16 MAC, __SMLALD).
 *
 * Rationale: on the MSP430FR5994 Capuchin offloads exactly this call to the LEA
 * vector accelerator via TI DSPLib. The closest Apollo4 analogue of "vendor DSP
 * library on the SoC's MAC hardware" is ARM CMSIS-DSP on the M4F's DSP
 * instructions -- the same instruction class the RUIC engine's NN_SIMD path
 * uses. Everything else Capuchin runs (im2col copies, shifts, adds, bias,
 * activation, pooling, flatten, layer copies) stays in upstream C, as on MSP430.
 *
 * Arithmetic: CMSIS-DSP 1.16.2 accumulates in 64 bits and stores
 * __SSAT((q31)(sum >> 15), 16). TI's generic C (dsplib_sw.c) accumulates in 32
 * bits and stores saturate(sum >> 15). The two are identical whenever the
 * 32-bit accumulation does not overflow; CAPUCHIN_NUMERIC_AUDIT on the host
 * counts such overflows (zero on the RUIC LeNet test set, see the V8 doc), and
 * the board self-check compares every output with the host reference.
 */
#include "DSPLib.h"

#if defined(CAPUCHIN_KERNEL) && CAPUCHIN_KERNEL == 2
#include "arm_math.h"

#ifndef CAPUCHIN_CMSIS_STATE_LEN
/* arm_mat_mult_q15 transposes B into pState (srcBRows*srcBCols q15). Capuchin
 * bounds every LEA operand by LEA_RAM_LENGTH (1892) before calling, so the same
 * bound covers B. Checked below rather than assumed. */
#define CAPUCHIN_CMSIS_STATE_LEN 1892
#endif
static q15_t s_cmsis_state[CAPUCHIN_CMSIS_STATE_LEN] __attribute__((aligned(4)));

msp_status msp_matrix_mpy_q15(const msp_matrix_mpy_q15_params *params, const _q15 *srcA, const _q15 *srcB, _q15 *dst)
{
    if (params->srcACols != params->srcBRows) {
        return MSP_SIZE_ERROR;
    }
    if ((uint32_t)params->srcBRows * params->srcBCols > CAPUCHIN_CMSIS_STATE_LEN) {
        return MSP_LEA_OUT_OF_RANGE;
    }
    arm_matrix_instance_q15 a, b, c;
    arm_mat_init_q15(&a, params->srcARows, params->srcACols, (q15_t *)srcA);
    arm_mat_init_q15(&b, params->srcBRows, params->srcBCols, (q15_t *)srcB);
    arm_mat_init_q15(&c, params->srcARows, params->srcBCols, (q15_t *)dst);
    return (arm_mat_mult_q15(&a, &b, &c, s_cmsis_state) == ARM_MATH_SUCCESS)
               ? MSP_SUCCESS : MSP_SIZE_ERROR;
}
#endif
