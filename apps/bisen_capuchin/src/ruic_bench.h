/* ruic_bench.h -- the RUIC engine (engine-r2a, OS package sources, unmodified)
 * behind a two-call API, so the same firmware times both engines with the same
 * harness, clock, cache state and instrumentation. */
#ifndef RUIC_BENCH_H
#define RUIC_BENCH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* nn_run(): nn_begin (copies the 1024-byte input) + every unit to completion. */
int ruic_infer(const int8_t *in, int8_t scores[10]);
const char *ruic_engine_desc(void);
#ifdef __cplusplus
}
#endif
#endif
