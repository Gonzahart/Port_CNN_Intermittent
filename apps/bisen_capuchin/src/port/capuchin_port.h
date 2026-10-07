/*
 * capuchin_port.h -- Apollo4 (Cortex-M4F) portability layer for Capuchin.
 *
 * Force-included (-include) into every upstream Capuchin translation unit.
 * It supplies only what the TI CCS toolchain provides implicitly and GCC does
 * not; it adds no arithmetic. Every numerical operation that Capuchin executes
 * stays in the upstream sources (src/capuchin/...) or, for the one operation
 * that upstream sends to the MSP430 LEA accelerator, in a TI DSPLib
 * implementation selected by CAPUCHIN_KERNEL (see DSPLib.h).
 *
 * Port changes are tagged in the sources:
 *   [PORT P1]  matrix_ops.c dma_load(): MSP430 DMA block copy -> CPU copy
 *              (inline loop, or memcpy with CAPUCHIN_COPY_LOOP=0)
 *   [PORT A1]  AveragePooling2D layer (class 6), not in upstream Capuchin
 *   [PORT H1]  decoder.c per-layer measurement hook, empty by default
 * The full diff against upstream 76b6eb2 is patches/capuchin-76b6eb2-apollo4.patch.
 */
#ifndef CAPUCHIN_PORT_H
#define CAPUCHIN_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>   /* upstream layers.c calls memset() without including it */

#define CAPUCHIN_PORT_APOLLO4 1

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prototypes for upstream functions that upstream calls before (or without)
 * declaring them. TI's compiler accepts implicit declarations; GCC >= 14
 * rejects them. Declaring them changes no behaviour.
 */
struct matrix;
int16_t *dma_load(int16_t *result, int16_t *data, uint16_t n);
struct matrix *matrix_multiply_reduce(struct matrix *result, struct matrix *mat1,
                                      struct matrix *mat2, uint16_t precision);
struct matrix *matrix_multiply_vanilla(struct matrix *result, struct matrix *mat1,
                                       struct matrix *mat2, uint16_t precision);
struct matrix *filter_im2col(struct matrix *result, struct matrix *input,
                             struct matrix *filter, uint16_t precision,
                             uint16_t stride_numRows, uint16_t stride_numCols);

/* [PORT H1] per-layer measurement hook (decoder.c, end of each layer). */
#ifdef CAPUCHIN_LAYER_PROFILE
void capuchin_port_layer_end(void);
#define CAPUCHIN_PORT_LAYER_END() capuchin_port_layer_end()
#else
#define CAPUCHIN_PORT_LAYER_END() do { } while (0)
#endif

/* Set if a DSPLib call returns an error status (upstream loops forever). */
extern volatile uint32_t g_capuchin_port_error;

#ifdef __cplusplus
}
#endif

#endif /* CAPUCHIN_PORT_H */
