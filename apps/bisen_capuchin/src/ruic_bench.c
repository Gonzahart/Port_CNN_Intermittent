/* ruic_bench.c -- see ruic_bench.h. Compiled with the OS package's engine flags
 * (NN_DATAFLOW=0 NN_SIMD=1 NN_MAX_CONV_IN_C=6, set in module.mk). */
#include <string.h>
#include "nn_engine.h"
#include "ruic_bench.h"

static nn_ctx_t s_ctx;

int ruic_infer(const int8_t *in, int8_t scores[10])
{
    int c = nn_run(&s_ctx, in);
    memcpy(scores, nn_scores(&s_ctx), 10);
    return c;
}

const char *ruic_engine_desc(void)
{
#if NN_DATAFLOW == 0
    return NN_SIMD ? "engine-r2a OS simd1" : "engine-r2a OS simd0";
#else
    return NN_SIMD ? "engine-r2a IS simd1" : "engine-r2a IS simd0";
#endif
}
