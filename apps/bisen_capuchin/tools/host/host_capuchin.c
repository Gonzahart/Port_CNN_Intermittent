/* Host runner for the SAME Capuchin sources the board builds.
 * stdin: N x 1024 int16 (Q5.10) inputs. stdout: 10 scores + label per line.
 * stderr: numeric audit counters (when built with CAPUCHIN_NUMERIC_AUDIT). */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "capuchin_invoke.h"
#ifdef CAPUCHIN_NUMERIC_AUDIT
#include "capuchin_audit.h"
#endif
int main(void) {
    int16_t in[CAPUCHIN_IN_LEN], s[CAPUCHIN_OUT_LEN];
    while (fread(in, sizeof(int16_t), CAPUCHIN_IN_LEN, stdin) == CAPUCHIN_IN_LEN) {
        capuchin_load_input(in);
        int c = capuchin_infer(s);
        for (int i = 0; i < CAPUCHIN_OUT_LEN; i++) printf("%d ", s[i]);
        printf("%d\n", c);
    }
#ifdef CAPUCHIN_NUMERIC_AUDIT
    fprintf(stderr, "audit mpy_outputs=%" PRIu64 " mpy_acc32_overflow=%" PRIu64 " mpy_out_saturated=%" PRIu64
            " shift_outputs=%" PRIu64 " shift_wrapped=%" PRIu64 "\n",
            g_capuchin_audit.mpy_outputs, g_capuchin_audit.mpy_acc32_overflow, g_capuchin_audit.mpy_out_saturated,
            g_capuchin_audit.shift_outputs, g_capuchin_audit.shift_wrapped);
#endif
    fprintf(stderr, "kernel=%s\n", capuchin_kernel_name());
    return 0;
}
