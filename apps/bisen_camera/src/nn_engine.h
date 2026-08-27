// nn_engine.h -- int8 inference for feed-forward networks.
//
// Replaces TFLM for the ops listed below. TFLM runs a model inside one atomic
// Invoke(); here the caller owns the loop, so inference runs in tiles and can
// stop between any two units and pick up later with the same result.
//
// Position is (layer, unit). What a unit is depends on NN_DATAFLOW below --
// one output element, or one input pixel. Either way the context struct is the
// whole state, and the position is enough to recompute everything else.
//
// The layer sequence comes from a const table the host exporter generates, so
// a different network built from the same ops needs no change here.
#ifndef NN_ENGINE_H
#define NN_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dataflow, selected at build time. Both produce identical results; they
// differ in where an inference can stop and what that costs. Each follows one
// of the two papers -- they are not two attempts at the same thing.
//
//   0  OUTPUT-STATIONARY -- what HAWAII describes. A unit is one output
//      element: its dot product runs to completion and the result is written
//      once, so no partial sum is ever live at a stop point. 8094 stop points
//      for this LeNet, and a checkpoint carries nothing beyond the input still
//      to be read and the output so far. HAWAII IV-D2 states the principle
//      outright -- partial sub-operation progress "resides in VM and will be
//      lost altogether... as if the incomplete suboperation has never been
//      executed" -- so there is nothing to store.
//
//   1  INPUT-STATIONARY -- what BISen describes. A unit is one input pixel,
//      scattered into every output it feeds. 2318 stop points, and many
//      outputs are part-summed at once, so a checkpoint must also carry an
//      accumulator band -- 3360 bytes for conv1. That band is not a defect of
//      this implementation: it is BISen's design. p.5 says a footprint holds
//      the layer, kernel and input indices "together with the corresponding
//      PARTIAL output feature-map values", and p.8 spends 30 of its 48
//      position bits on the (w,h) of *an input*. Storing partial ofmaps is
//      exactly where BISen extends HAWAII, and their approximate MSB-first
//      store (our `acc_drop`) is what pays for the size.
//
// Not yet implemented: BISen's 10-bit kernel index, which would let mode 1
// stop between output channels of one input pixel -- mid-kernel-sweep. Our
// unit finishes all channels, so we are one dimension coarser than BISen.
#define NN_DATAFLOW_OUTPUT 0
#define NN_DATAFLOW_INPUT  1
#ifndef NN_DATAFLOW
#define NN_DATAFLOW NN_DATAFLOW_OUTPUT
#endif

// Ops. To add one: an id here, a kernel in nn_kernels.c, a row in the registry
// at the bottom of that file, and a descriptor from the exporter.
//
// A kernel must fill output units [u0, u0+n) from its input buffer and its
// descriptor, nothing else. That is what makes it stoppable. Conv, pool, dense
// and elementwise all qualify; anything with state carried across units (RNNs)
// does not.
enum {
    NN_OP_CONV = 0,  // square kernel, valid padding, stride 1
    NN_OP_POOL,      // average pooling, stride == pool size
    NN_OP_FC,        // fully connected
    NN_OP_COUNT
};

// One layer, as emitted by export_weights.py. Everything the kernel needs,
// decided on the host: shapes, quantization, which buffer to read and write,
// and how many units the layer divides into.
typedef struct {
    uint8_t op;
    uint8_t in_buf, out_buf;  // buffer indices, assigned by the host planner
    uint16_t units;           // output rows (conv/pool) or neurons (fc)
    uint16_t in_h, in_w, in_c;
    uint16_t out_h, out_w, out_c;
    uint8_t k;                // kernel / pool size; unused for fc
    // Low bits of each accumulator a checkpoint may discard without moving the
    // output. Worked out on the host from the layer's own requantization: an
    // absolute truncation error of 2^n arrives at the output divided by the
    // requantizer's scale, so a layer that divides by ~968 can lose 8 bits for
    // under half an LSB, while average pooling divides by only k*k and can
    // lose almost none. Derived, not tuned -- see the exporter.
    uint8_t acc_drop;
    int16_t in_zp, out_zp, act_min;
    const int8_t *w;
    const int32_t *b, *mult, *shift;
} nn_layer_t;

// The generated table and the sizes derived from it. Overridable so a second
// model can be built alongside the first:
//   gcc -DNN_WEIGHTS_HEADER='"other_weights.h"' ...
#ifndef NN_WEIGHTS_HEADER
#define NN_WEIGHTS_HEADER "lenet_weights.h"
#endif
#include NN_WEIGHTS_HEADER

// Accumulator band: the part-summed output rows an input-stationary layer keeps
// open. k rows for convolution, one for pooling, none for dense; the exporter
// emits the largest any layer needs. Older generated headers predate it.
#ifndef NN_ACC_LEN
#define NN_ACC_LEN 1024   /* int32 entries; nn_begin checks this is enough */
#endif

// The whole state of one inference. No arena, no interpreter, no malloc.
// Self-contained, so it can be copied or stored and resumed from.
typedef struct {
    uint16_t layer;  // which layer is in progress; == NN_NUM_LAYERS when done
    uint16_t unit;   // next unit to consume within that layer (<= in_h*in_w)
    int8_t buf0[NN_BUF0_SZ];
    int8_t buf1[NN_BUF1_SZ];
#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    int32_t acc[1];            // unused: no partial sum is ever live
#else
    int32_t acc[NN_ACC_LEN];   // part-accumulated output rows; see nn_kernels.c
#endif
} nn_ctx_t;

// Load an input and rewind to the first layer. Returns 0, or -1 if this build's
// NN_ACC_LEN is too small for the model's widest layer -- which means the
// weights header and the engine disagree, so nothing is run.
int nn_begin(nn_ctx_t *c, const int8_t *in);

// Accumulator entries the widest layer needs. NN_ACC_LEN must be at least this;
// the exporter emits it. Diagnostic, and what nn_begin checks against.
uint32_t nn_acc_required(void);

// Do at most max_units units. Returns 1 when the network is done, 0 if there
// is more left. max_units sets how long the engine holds the CPU.
int nn_step(nn_ctx_t *c, int max_units);

// Run to completion and return the predicted class.
int nn_run(nn_ctx_t *c, const int8_t *in);

// Valid once nn_step has returned 1.
int nn_argmax(const nn_ctx_t *c);
const int8_t *nn_scores(const nn_ctx_t *c);

// True if this context was stopped partway and still has work left.
int nn_in_progress(const nn_ctx_t *c);

// Mark a context as no longer in progress, discarding whatever position it
// held. For a caller that has decided a partially-done inference is not worth
// finishing -- after which nn_in_progress() is false and anything asking "what
// is live?" correctly reports nothing.
//
// This exists because "in progress" is the engine's own representation
// (currently layer < NN_NUM_LAYERS) and nobody outside should be writing that
// field to fake it.
void nn_abandon(nn_ctx_t *c);

// Granularity the live region is reported at. Storage backends program in
// fixed-size blocks -- 16 bytes on the Apollo4's MRAM -- so the input pointer is
// held on a multiple of this rather than trimmed to the exact byte.
#ifndef NN_LIVE_ALIGN
#define NN_LIVE_ALIGN 16
#endif

// Which part of the context currently means anything: the part of the layer's
// input still to be read, plus the output units done so far. The rest is stale,
// and a resume with that region set to garbage gives the same answer.
//
// `in` is NOT the start of the buffer. Input rows the layer has already
// consumed are excluded -- with stride 1 a conv finishes with row r once output
// row r is out, and a pool with stride k finishes r*k -- so `in` advances
// through the buffer as the layer progresses. Dense layers are the exception
// and always report the whole vector, since every neuron reads all of it.
//
// The bounds come from the descriptor table, so only the engine can work them
// out, and they are a pure function of (layer, unit) -- which is why a
// checkpoint can recompute them on restore without storing any lengths.
// Ranges from about 4.7 kB mid-conv1 down to 128 bytes in the last dense layer
// -- a 37x span. Use it to avoid copying bytes that do not matter.
typedef struct {
    int8_t *in;
    uint32_t in_len;
    int8_t *out;
    uint32_t out_len;   // only the output rows already finished
    int32_t *acc;
    uint32_t acc_len;   // the open accumulator band; 0 for dense, 0 at unit 0
} nn_live_t;

void nn_live(nn_ctx_t *c, nn_live_t *lv);
uint32_t nn_live_bytes(nn_ctx_t *c);

// Progress, for reporting only. Computed from the descriptor table rather than
// read from it, so a header generated before the dataflow change still agrees.
uint32_t nn_total_units(void);
uint32_t nn_units_done(const nn_ctx_t *c);

#ifdef __cplusplus
}
#endif
#endif  // NN_ENGINE_H
