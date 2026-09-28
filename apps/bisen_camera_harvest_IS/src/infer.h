#ifndef INFER_H
#define INFER_H

#include <stdint.h>

// Driving the engine, and the checkpoint glue around it.
//
// Owns the inference context, the scores and the timing. The application asks
// for a frame to be classified and does not deal with the engine directly.

#ifdef __cplusplus
extern "C" {
#endif

// Restore a checkpoint if one is pending, and report what was found. Call once
// at startup, after ckpt_init(). Returns non-zero if an inference is waiting to
// be finished, in which case call infer_finish() before scanning anything.
int  infer_init(void);

// Stepwise inference, for a caller that owns the schedule.
//
// infer_classify() below runs a whole inference and decides for itself when to
// pause. These two let something else drive: prepare a frame, then advance it a
// bounded number of units at a time. Between any two infer_step() calls the
// engine state is coherent and workload.h's wl_state() describes it exactly.
//
// infer_begin returns 0 on success. infer_step returns 1 when the inference is
// finished, at which point infer_scores() and the digit are valid.
int infer_begin(const uint16_t raw[1024]);
int infer_step(uint32_t max_units);
int infer_digit(void);

// Preprocess, load and run to completion. Returns the digit.
int  infer_classify(const uint16_t raw[1024]);

// Finish an inference that a reset interrupted. The engine picks up at the
// layer and unit it stopped at; no scan and no preprocessing happen.
int  infer_finish(void);

// The engine context, for the checkpoint-state API in workload.h. Exposed so a
// storage layer can be told which bytes are live without this module knowing
// anything about storage. Do NOT drive the engine through it -- infer_classify
// and infer_finish own the stepping.
#include "nn_engine.h"
nn_ctx_t *infer_ctx(void);

// The last inference's 10 raw logits, and their softmax as parts per thousand.
const int8_t *infer_scores(void);
void infer_probs_per_mille(int out[10]);

// Timing of the last inference and the session, in microseconds.
uint32_t infer_last_us(void);
uint32_t infer_avg_us(void);
uint32_t infer_min_us(void);
uint32_t infer_max_us(void);
uint32_t infer_count(void);

// Where this session resumed from, or -1 if it started cold. Worth reprinting
// every frame: after a power cut the serial link is gone, so this is the only
// way to learn how the session started once you reconnect.
int      infer_boot_resume_layer(void);
int      infer_boot_resume_unit(void);
uint32_t infer_boot_resume_seq(void);
uint32_t infer_boot_resume_crc(void);

#ifdef __cplusplus
}
#endif

#endif  // INFER_H
