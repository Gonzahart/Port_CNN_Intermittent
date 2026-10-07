/*
 * main_xchk.c -- OPTIONAL replacement for capuchin-MCU/main.c on the native
 * MSP430FR5994 (TI CCS), for gate X1: does native Capuchin (LEA) produce the
 * same ten Q5.10 logits as the Apollo4 port, vector for vector?
 *
 * Setup (upstream Capuchin 76b6eb2 CCS project):
 *   1. git apply capuchin-76b6eb2-avgpool.patch          (adds AveragePooling2D)
 *   2. copy neural_network_parameters.h and capuchin_vectors.h into capuchin-MCU/
 *   3. replace main.c with this file; build; run; halt at the final
 *      __no_operation() and read xchk_pass / xchk_first_fail / xchk_scores.
 *
 * The body of the loop is upstream main.c unchanged. P1.0 is high during each
 * apply_model()+argmax() for a scope/EnergyTrace timing window; it is not part
 * of the cross-check. Timing/energy belong to the colleague's own MSP430
 * harness; this file only establishes numeric equivalence. Unlike the Apollo4
 * capuchin_invoke timing, the P1.0 window excludes filling input_buffer.
 *
 * NOT YET BUILT: written against upstream main.c/main.h but never compiled in
 * CCS. FRAM fit with the extra ~17 KB of vectors is expected (MODEL_ARRAY ends at
 * 0x3627C) but unverified -- check the CCS memory map.
 */
#include "main.h"
#include "capuchin_vectors.h"

#pragma PERSISTENT(xchk_scores)
int16_t xchk_scores[XCHK_N][OUTPUT_NUM_LABELS] = {{0}};
#pragma PERSISTENT(xchk_labels)
int16_t xchk_labels[XCHK_N] = {0};
#pragma PERSISTENT(xchk_pass)
volatile uint16_t xchk_pass = 0;          /* vectors whose 10 scores AND label match */
#pragma PERSISTENT(xchk_first_fail)
volatile int16_t xchk_first_fail = -1;    /* index of first mismatching vector, -1 if none */

void main(void){
    uint16_t v, i, ok;

    /* stop watchdog timer */
    WDTCTL = WDTPW | WDTHOLD;
    init_gpio();
    init_clock_system();

    xchk_pass = 0;
    xchk_first_fail = -1;
    for (v = 0; v < XCHK_N; v++) {
        for (i = 0; i < INPUT_LENGTH; i++) {
            input_buffer[i] = XCHK_PIX_TO_Q10[XCHK_PIX[v * INPUT_LENGTH + i]];
        }

        P1OUT |= BIT0;
        /* ---- upstream main.c ---- */
        inputFeatures.numRows = INPUT_NUM_ROWS;
        inputFeatures.numCols = INPUT_NUM_COLS;
        inputFeatures.data = input_buffer;
        outputLabels.numRows = OUTPUT_NUM_LABELS;
        outputLabels.numCols = LEA_RESERVED;
        outputLabels.data = output_buffer;
        apply_model(&outputLabels, &inputFeatures);
        label = argmax(&outputLabels);
        /* ------------------------- */
        P1OUT &= ~BIT0;

        ok = (label == XCHK_EXPECTED_LABEL[v]);
        for (i = 0; i < OUTPUT_NUM_LABELS; i++) {
            xchk_scores[v][i] = outputLabels.data[i * outputLabels.numCols];
            if (xchk_scores[v][i] != XCHK_EXPECTED_SCORES[v * OUTPUT_NUM_LABELS + i]) ok = 0;
        }
        xchk_labels[v] = label;
        if (ok) xchk_pass++;
        else if (xchk_first_fail < 0) xchk_first_fail = (int16_t)v;
    }

    __no_operation();   /* breakpoint here: expect xchk_pass == XCHK_N */
    while (1) { }
}
