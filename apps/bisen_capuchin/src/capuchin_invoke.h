/* capuchin_invoke.h -- the upstream main.c inference sequence as a callable API. */
#ifndef CAPUCHIN_INVOKE_H
#define CAPUCHIN_INVOKE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CAPUCHIN_IN_LEN 1024     /* 32x32x1, Q5.10, CHW (== HW for one channel) */
#define CAPUCHIN_OUT_LEN 10

/* Place one input in Capuchin's input_buffer (upstream: a FRAM-persistent array
 * the application fills before main() runs the model). Not part of inference. */
void capuchin_load_input(const int16_t *in_q10);

/* Exactly upstream main.c after clock/GPIO init: set up inputFeatures and
 * outputLabels, apply_model(), argmax(). Copies the 10 Q5.10 logits out. */
int capuchin_infer(int16_t scores[CAPUCHIN_OUT_LEN]);

const char *capuchin_kernel_name(void);
#ifdef __cplusplus
}
#endif
#endif
