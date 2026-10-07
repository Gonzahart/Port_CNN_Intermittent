/*
 * capuchin_port.c -- pieces of the port that are not DSPLib.
 */
#include "capuchin_port.h"

#ifndef CAPUCHIN_KERNEL
#define CAPUCHIN_KERNEL 1
#endif

#if CAPUCHIN_KERNEL == 0
/*
 * CAPUCHIN_KERNEL=0 builds upstream's non-MSP ("#else") code paths. Upstream
 * defines dma_load() only under IS_MSP, yet decoder.c and layers.c call it on
 * every path, so the upstream non-MSP configuration does not link as shipped.
 * Supply the same n-word copy as [PORT P1], in the same two forms.
 */
int16_t *dma_load(int16_t *result, int16_t *data, uint16_t n)
{
#if !defined(CAPUCHIN_COPY_LOOP) || CAPUCHIN_COPY_LOOP
    uint16_t i;
    for (i = 0; i < n; i++) {
        result[i] = data[i];
    }
    return result;
#else
    return (int16_t *)memcpy(result, data, (size_t)n * sizeof(int16_t));
#endif
}
#endif
