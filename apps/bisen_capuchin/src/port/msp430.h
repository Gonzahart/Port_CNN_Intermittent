/*
 * msp430.h -- stand-in for TI's device header on Apollo4.
 *
 * Upstream Capuchin includes <msp430.h> (and "DSPLib.h") whenever IS_MSP is
 * defined. On Apollo4 nothing from the device header is used by the code that
 * executes: DMA registers appear only in the original dma_load() body, which
 * [PORT P1] compiles out. This header therefore only pulls in the port layer.
 */
#ifndef CAPUCHIN_MSP430_STANDIN_H
#define CAPUCHIN_MSP430_STANDIN_H
#include "capuchin_port.h"
#endif
