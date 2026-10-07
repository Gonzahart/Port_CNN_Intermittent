/*
 * capuchin_invoke.c -- upstream capuchin-MCU/main.c, minus the MSP430 watchdog,
 * GPIO and clock initialisation (board bring-up is done by the application).
 * Compiled as a Capuchin translation unit (same flags, port header forced in).
 */
#include "neural_network_parameters.h"
#include "math/matrix.h"
#include "decoder/decoder.h"
#include "../capuchin_invoke.h"

void capuchin_load_input(const int16_t *in_q10)
{
    memcpy(input_buffer, in_q10, sizeof(input_buffer));
}

int capuchin_infer(int16_t scores[CAPUCHIN_OUT_LEN])
{
    /* ---- upstream main.c ---- */
    inputFeatures.numRows = INPUT_NUM_ROWS;
    inputFeatures.numCols = INPUT_NUM_COLS;
    inputFeatures.data = input_buffer;

    outputLabels.numRows = OUTPUT_NUM_LABELS;
    outputLabels.numCols = LEA_RESERVED;   // one more column is reserved for LEA
    outputLabels.data = output_buffer;

    apply_model(&outputLabels, &inputFeatures);
    label = argmax(&outputLabels);
    /* ------------------------- */

    for (int i = 0; i < CAPUCHIN_OUT_LEN; i++) {
        scores[i] = outputLabels.data[i * outputLabels.numCols];
    }
    return label;
}

const char *capuchin_kernel_name(void)
{
#if !defined(CAPUCHIN_KERNEL) || CAPUCHIN_KERNEL == 1
    return "dsplib-sw";     /* LEA path, TI DSPLib generic C */
#elif CAPUCHIN_KERNEL == 2
    return "cmsis-dsp";     /* LEA path, CMSIS-DSP on M4 DSP extension */
#else
    return "cpu-ref";       /* upstream non-MSP C path */
#endif
}
