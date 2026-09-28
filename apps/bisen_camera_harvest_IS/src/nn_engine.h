// nn_engine.h -- int8 inference for feed-forward networks.
//
// engine-080: frozen engine + 063 packed scalar + 065 int8 SIMD + 071 table CRC.
// Activations remain int8. Packed weights use the unchanged 063 decoder.
// NN_DATAFLOW defaults to OS; NN_SIMD defaults to on (emulated on host).
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

// Low-bit weight storage (engine-063-wo-v1). Included BEFORE the weights
// header: a packed header refuses to compile unless NN_WO_VERSION is defined, so
// it cannot be built against an engine that would read its bytes as int8.
#include "nn_wo.h"

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
#include "nn_build.h"

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
    // Weight storage (engine-063-wo-v1; format in nn_wo.h). LAST on purpose: the
    // frozen exporter's positional initialisers stop at `shift`, so its headers
    // leave both at 0 -- a legacy int8 array -- and run unchanged.
    //   w_bits  0 legacy int8, 8 int8 declared, 2..7 packed at that width
    //   w_len   bytes `w` points to; 0 in a legacy descriptor
    uint8_t w_bits;
    uint32_t w_len;
} nn_layer_t;

// The generated table and the sizes derived from it. Overridable so a second
// model can be built alongside the first:
//   gcc -DNN_WEIGHTS_HEADER='"other_weights.h"' ...
#ifndef NN_WEIGHTS_HEADER
#define NN_WEIGHTS_HEADER "lenet_weights.h"
#endif
// A header from the frozen exporter ends each initialiser at `shift`, so w_bits
// and w_len are zero -- "legacy int8" -- by design. -Wextra calls that a missing
// initialiser; it is the compatibility mechanism, so the warning is silenced for
// the generated header only.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include NN_WEIGHTS_HEADER
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// IS SIMD capacity is based on maximum CONV INPUT channels, never outputs.
// Legacy headers do not provide it and retain the 065 capacity of 64.
// A smaller explicit override is legal: affected layers report scalar-cmax.
// The range guard and rounded allocation also support CMAX 1, 2 and 3.
#ifdef NN_MAX_CONV_IN_C
#if NN_MAX_CONV_IN_C < 0 || NN_MAX_CONV_IN_C > 65535
#error "NN_MAX_CONV_IN_C must be in 0..65535"
#endif
#endif
#ifndef NN_SIMD_IS_CMAX
#ifdef NN_MAX_CONV_IN_C
#define NN_SIMD_IS_CMAX ((NN_MAX_CONV_IN_C) > 0 ? (NN_MAX_CONV_IN_C) : 1)
#else
#define NN_SIMD_IS_CMAX 64
#endif
#endif
#if NN_SIMD_IS_CMAX < 1 || NN_SIMD_IS_CMAX > 65535
#error "NN_SIMD_IS_CMAX must be in 1..65535"
#endif

// A dispatch report, not a claim that every MAC uses a DSP instruction. The
// int8 optimized IS path includes 065's scalar single-channel specialization;
// pooling has no DSP MAC path. Query before execution; no context is modified.
typedef enum {
    NN_PATH_INVALID = 0,
    NN_PATH_PACKED_SCALAR,
    NN_PATH_SCALAR_DISABLED,
    NN_PATH_SCALAR_OFFSET,
    NN_PATH_SCALAR_CMAX,
    NN_PATH_INT8_SIMD,
    NN_PATH_POOL_SCALAR,
    NN_PATH_POOL_OPTIMIZED
} nn_path_t;
nn_path_t nn_layer_path(uint32_t layer);
const char *nn_path_name(nn_path_t path);
uint32_t nn_simd_is_cmax(void);
const char *nn_simd_implementation(void);

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

// Load an input and rewind to the first layer. Returns 0, or -1 if the model is
// inadmissible for this build -- in which case nothing is run. Three refusals:
//
//   * NN_ACC_LEN is too small for the model's widest layer, so the weights
//     header and the engine disagree. Without this check a kernel writes past
//     the accumulator array (observed: ASan global-buffer-overflow in k_conv).
//   * Some layer has more than 65535 units, which c->unit cannot represent.
//     Without this check the model is accepted and then never completes: the
//     cursor wraps mid-layer, so the layer's completion test never fires.
//   * Some layer's weight storage is not admissible (nn_weights_admissible):
//     a width outside {0, 2..8}, a length that disagrees with the shape and
//     width, a width on a layer without weights, the unused code, or nonzero
//     padding. The kernels decode unchecked, so they may only run after this.
int nn_begin(nn_ctx_t *c, const int8_t *in);

// Weight storage (engine-063-wo-v1). Pure functions of the const table.
//
//   nn_weight_count(L)      elements in the layer's weight array; 0 for pooling
//   nn_weight_bytes(L)      bytes those elements occupy as stored: w_len when
//                           packed, else the element count
//   nn_weight(L, i)         element i, decoded. Only after admission; i must be
//                           below nn_weight_count(L)
//   nn_weights_packed()     1 if any layer is packed (widths 2..7)
//   nn_weights_admissible() 1 if every layer passes the checks listed under
//                           nn_begin. Checks each element once, the first time
//                           it is asked, and keeps the answer: the table is
//                           const, so the answer cannot change.
uint32_t nn_weight_count(const nn_layer_t *L);
uint32_t nn_weight_bytes(const nn_layer_t *L);
int32_t nn_weight(const nn_layer_t *L, uint32_t i);
int nn_weights_packed(void);
int nn_weights_admissible(void);

// Accumulator entries the widest layer needs. NN_ACC_LEN must be at least this;
// the exporter emits it. Diagnostic, and what nn_begin checks against.
uint32_t nn_acc_required(void);

// Do at most max_units units. Returns 1 when the network is done, 0 if there
// is more left. max_units sets how long the engine holds the CPU.
//
// engine-063-wo-v1: returns -1, and runs nothing, if the weight table fails
// admission (nn_weights_admissible) -- whether or not the caller honoured
// nn_begin's refusal. -1 is non-zero, so a `while (!nn_step(...))` loop ends.
int nn_step(nn_ctx_t *c, int max_units);

// Run to completion and return the predicted class.
int nn_run(nn_ctx_t *c, const int8_t *in);

// Valid once nn_step has returned 1.
int nn_argmax(const nn_ctx_t *c);
const int8_t *nn_scores(const nn_ctx_t *c);

// True if this context was stopped partway and still has work left.
int nn_in_progress(const nn_ctx_t *c);

// Mark a context as no longer in progress, discarding whatever position it
// held (the coworker's app calls this; added for it, 2026-09-28, from the
// reviewed job-5 draft). The cursor becomes the canonical completion position
// (NN_NUM_LAYERS, 0), which nn_position_valid() accepts. NULL is ignored.
// An abandoned context is shaped like a finished one: nn_live() reports the
// NN_OUT_LEN output bytes and nn_units_done() every unit, but those bytes are
// not a result -- do not read nn_scores() or nn_argmax() after abandoning.
// Trap: a zero-filled context is (layer 0, unit 0), which reads as IN PROGRESS
// until nn_begin() or nn_abandon() is called.
void nn_abandon(nn_ctx_t *c);

// Granularity the live region is reported at. Storage backends program in
// fixed-size blocks -- 16 bytes on the Apollo4's MRAM -- so the
// OUTPUT-STATIONARY input pointer is held on a multiple of this rather than
// trimmed to the exact byte.
//
// It applies to that mode ONLY. Input-stationary retires at the exact pixel
// frontier and reports an unaligned pointer (see dead_in_bytes in
// nn_engine.c); the serializer handles the partial block. Do not set this to 1
// in order to get pixel-exact behaviour -- the macro is shared, and the
// exporter's NN_LIVE_MAX assumes this value for the output-stationary walk.
#ifndef NN_LIVE_ALIGN
#define NN_LIVE_ALIGN 16
#endif

// NN_LIVE_MAX was computed by the exporter against one assumed alignment, and
// the #ifndef above means a -DNN_LIVE_ALIGN=32 is legal and would silently
// invalidate it -- the engine would keep more input per layer than the slot was
// sized for, and saves would start failing on the device. The exporter records
// what it assumed; check it here, which is the only point that sees both the
// generated value and a command-line override. Headers that predate
// NN_LIVE_ALIGN_ASSUMED simply skip the check.
#ifdef NN_LIVE_ALIGN_ASSUMED
typedef char nn_live_align_matches_exporter
    [(NN_LIVE_ALIGN == NN_LIVE_ALIGN_ASSUMED) ? 1 : -1];
#endif

// Which part of the context currently means anything: the part of the layer's
// input still to be read, plus the output units done so far. The rest is stale,
// and a resume with that region set to garbage gives the same answer.
//
// `in` is NOT the start of the buffer. What is excluded depends on the
// dataflow, because the two modes know different things about what has been
// consumed:
//
//   OUTPUT-STATIONARY -- whole input ROWS the layer has finished with: with
//   stride 1 a conv finishes with row r once output row r is out, and a pool
//   with stride k finishes r*k. Rounded down to NN_LIVE_ALIGN.
//
//   INPUT-STATIONARY -- the EXACT pixel frontier. A unit is one input pixel
//   and it is dead once scattered, so `in` is buf + unit*in_c and `in_len` is
//   the exact remaining suffix. No row rounding, no alignment rounding, so
//   this pointer is usually unaligned. Nothing before it is reported, stored,
//   or needed on resume.
//
// Dense is the exception in BOTH modes and always reports the whole vector,
// since every neuron reads all of it.
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

// Whether (layer, unit) is a position this build can be stopped at, and
// therefore one nn_live() may be asked about. Anything reading a position back
// from storage must pass it through this BEFORE deriving regions or writing to
// a context: nn_live() turns `unit` into a length, so an invalid one is an
// out-of-bounds write, not merely a wrong answer. Returns 1 if valid.
// Completion is exactly (NN_NUM_LAYERS, 0).
int nn_position_valid(uint32_t layer, uint32_t unit);

void nn_live(nn_ctx_t *c, nn_live_t *lv);
uint32_t nn_live_bytes(nn_ctx_t *c);

// Progress, for reporting only. Computed from the descriptor table rather than
// read from it, so a header generated before the dataflow change still agrees.
uint32_t nn_total_units(void);
uint32_t nn_units_done(const nn_ctx_t *c);

// The requantization policy the kernels were compiled with: conv in bits 8-15,
// dense in bits 0-7, each NN_RQ_SINGLE or NN_RQ_DOUBLE from nn_quant.h. Two
// builds that differ here produce different logits from the same model, so the
// checkpoint seed hashes it (see ckpt_cfg_seed). Defined in nn_kernels.c.
uint32_t nn_requant_policy(void);

#ifdef __cplusplus
}
#endif
#endif  // NN_ENGINE_H
