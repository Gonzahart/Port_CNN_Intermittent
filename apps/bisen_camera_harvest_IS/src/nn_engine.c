// nn_engine.c -- the table walker and the API. See nn_engine.h.
//
// The decoder: reads the descriptor table, dispatches to the kernels in
// nn_kernels.c. Same job as Capuchin's decoder.c, with two differences. The
// table is a typed const struct instead of an untyped array read with pointer
// arithmetic, so nothing is parsed at run time and the compiler checks the
// fields. And the walk starts at c->layer, not zero, so it resumes.
//
// The only translation unit that instantiates the weights and the table.
// Elsewhere the header gives an extern declaration.
#define NN_WEIGHTS_IMPL
#include "nn_engine.h"
#include "nn_kernels.h"

static int8_t *buf_of(nn_ctx_t *c, int idx) {
    return idx ? c->buf1 : c->buf0;
}

// How a layer divides into units. Conv and pool are input-stationary, so a unit
// is an input pixel; dense is output-stationary, so it is a neuron.
//
// Derived here rather than read from the descriptor's `units` field, so that a
// weights header generated before the dataflow changed still runs correctly.
static uint32_t units_of(const nn_layer_t *L) {
    if (L->op == NN_OP_FC) return (uint32_t)L->out_c;
#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    // One output element. The index is also the offset of that element in the
    // output buffer, which is what keeps everything else so simple.
    return (uint32_t)L->out_h * L->out_w * L->out_c;
#else
    return (uint32_t)L->in_h * L->in_w;      // one input pixel
#endif
}

// Is (layer, unit) a position this build can actually be stopped at?
//
// Restore reads both fields out of storage, and nn_live() turns `unit` into a
// LENGTH -- out_bytes() returns it unclamped in output-stationary -- so an
// unchecked unit becomes an out-of-bounds write into the context before the CRC
// is ever consulted. The bound has to be applied before regions are derived,
// not after they are used.
//
// Derived from units_of(), so it follows NN_DATAFLOW automatically and never
// trusts the descriptor's `units` field: a header generated under the other
// dataflow carries a different count there, and that field is exactly what
// units_of() exists to avoid reading.
//
// Completion is exactly (NN_NUM_LAYERS, 0). nn_step advances the layer and
// zeroes the unit in the same call, so no other finished form can occur -- and
// for the same reason a position AT a layer's unit count is unreachable, hence
// strictly-less-than rather than <=.
int nn_position_valid(uint32_t layer, uint32_t unit) {
    if (layer == NN_NUM_LAYERS) return unit == 0;   // canonical completion
    if (layer > NN_NUM_LAYERS) return 0;
    if (unit >= units_of(&k_layers[layer])) return 0;
    // A unit the context cannot hold is not a position: nn_ctx_t.unit is 16 bit,
    // so a larger value would silently become a different one once stored.
    //
    // REDUNDANT for every model this engine currently builds: the check above
    // already rejects anything >= the layer's unit count, and LeNet's largest
    // layer has 4704 units. A mutation control confirmed deleting this line
    // fails no test, so the suite's 65536 / 0xFFFFFF cases pass on that bound,
    // not on this one.
    //
    // Kept as explicit representability defence -- a position that cannot be
    // stored is not a position. It does not, on its own, prevent the wrap: this
    // is a restore-time predicate, while the wrap happens during execution
    // because nn_step increments c->unit before testing layer completion. The
    // model is refused at admission instead -- see the unit-count guard in
    // nn_begin, which rejects any layer of more than 65535 units before
    // anything runs. Untested by construction; do not read the passing suite as
    // evidence that this line works.
    if (unit > 0xFFFFu) return 0;
    return 1;
}

// Output rows an input-stationary layer keeps part-accumulated at once. Conv
// windows overlap by k-1 rows, so k are open; pooling windows do not overlap,
// so exactly one is. Output-stationary keeps none: every dot product finishes
// before the next begins.
static uint32_t acc_rows(const nn_layer_t *L) {
#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    (void)L; return 0;
#else
    if (L->op == NN_OP_CONV) return L->k;
    if (L->op == NN_OP_POOL) return 1;
    return 0;
#endif
}

// Size of that band, in int32 entries.
static uint32_t acc_len_of(const nn_layer_t *L) {
    return acc_rows(L) * (uint32_t)L->out_w * L->out_c;
}

uint32_t nn_acc_required(void) {
    uint32_t worst = 0;
    for (int i = 0; i < NN_NUM_LAYERS; i++) {
        uint32_t n = acc_len_of(&k_layers[i]);
        if (n > worst) worst = n;
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Weight storage (engine-063-wo-v1). The format is in nn_wo.h.
// ---------------------------------------------------------------------------

uint32_t nn_weight_count(const nn_layer_t *L) {
    if (L->op == NN_OP_CONV) return (uint32_t)L->k * L->k * L->in_c * L->out_c;
    if (L->op == NN_OP_FC) return (uint32_t)L->in_c * L->out_c;
    return 0;
}

uint32_t nn_weight_bytes(const nn_layer_t *L) {
    return NN_WO_LAYER_PACKED(L) ? L->w_len : nn_weight_count(L);
}

int32_t nn_weight(const nn_layer_t *L, uint32_t i) {
    return NN_WO_LAYER_PACKED(L) ? nn_wo_raw((const uint8_t *)L->w, L->w_bits, i)
                                 : (int32_t)L->w[i];
}

int nn_weights_packed(void) {
    for (int i = 0; i < NN_NUM_LAYERS; i++) {
        if (NN_WO_LAYER_PACKED(&k_layers[i])) return 1;
    }
    return 0;
}

// One layer's storage against the format. A legacy descriptor (w_bits 0) is
// accepted exactly as before -- nothing about it is new -- provided it does not
// also claim a length. Everything else must be self-consistent: a declared width
// needs weights to describe, the length must be what the shape and width imply,
// and a packed array must hold no unused code and no nonzero padding.
//
// A length LONGER than the real array cannot be detected from here -- that is
// the one thing C cannot see -- which is why the exporter emits w_len as
// sizeof() of the array it describes.
static int layer_storage_ok(const nn_layer_t *L) {
    const uint32_t n = nn_weight_count(L);
    if (L->w_bits == 0) return L->w_len == 0;
    if (L->op == NN_OP_POOL || n == 0 || !L->w) return 0;
    if (L->w_bits == 8) return L->w_len == n;
    if (!NN_WO_LAYER_PACKED(L)) return 0;               // 1, or 9 and up
    const uint8_t *p = (const uint8_t *)L->w;
    if (L->w_len != nn_wo_len(n, L->w_bits)) return 0;
    if (!nn_wo_padding_zero(p, L->w_len, L->w_bits, n)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v;
        if (nn_wo_get(p, L->w_len, L->w_bits, n, i, &v) != 0) return 0;
    }
    return 1;
}

// 0 = not checked yet, 1 = admissible, -1 = refused. The table is const, so the
// first answer is the only answer; keeping it saves a pass over every packed
// element per inference. Dispatch and its diagnostic path are derived in the
// same pass. None of this const-table-derived state belongs in a checkpoint.
static int8_t g_weights_state;
static nn_kernel_fn g_layer_kernels[NN_NUM_LAYERS];
static nn_path_t g_layer_paths[NN_NUM_LAYERS];

int nn_weights_admissible(void) {
    if (!g_weights_state) {
        int8_t st = 1;
        for (int i = 0; i < NN_NUM_LAYERS; i++) {
            g_layer_kernels[i] = nn_select_kernel(&k_layers[i], &g_layer_paths[i]);
            if (st > 0 && !layer_storage_ok(&k_layers[i])) st = -1;
        }
        g_weights_state = st;
    }
    return g_weights_state > 0;
}

nn_path_t nn_layer_path(uint32_t layer) {
    if (layer >= NN_NUM_LAYERS) return NN_PATH_INVALID;
    (void)nn_weights_admissible();
    return g_layer_paths[layer];
}

int nn_begin(nn_ctx_t *c, const int8_t *in) {
    // The kernels decode packed weights unchecked; they may run only on a table
    // whose every descriptor and every code has been proved valid.
    if (g_weights_state != 1 && !nn_weights_admissible()) return -1;

    // The band has to hold the widest layer's open rows. A generated header
    // states this; refuse loudly rather than scribble past the array.
    if (nn_acc_required() > NN_ACC_LEN) return -1;

    // c->unit is 16 bit, and nn_step adds the whole tile before testing the
    // layer's count -- a layer of more than 65535 units wraps and never ends.
    for (int i = 0; i < NN_NUM_LAYERS; i++) {
        if (units_of(&k_layers[i]) > 0xFFFFu) return -1;
    }

    int8_t *dst = buf_of(c, k_layers[0].in_buf);
    for (int i = 0; i < NN_IN_LEN; i++) dst[i] = in[i];
    c->layer = 0;
    c->unit = 0;
    return 0;
}

int nn_step(nn_ctx_t *c, int max_units) {
    // Not only nn_begin: a caller that ignored its -1 must still never reach the
    // unchecked kernels on a table that failed admission (task 064, finding F1).
    // One flag test on the admitted hot path; initialize/refuse only when cold.
    if (g_weights_state != 1 && !nn_weights_admissible()) return -1;
    if (max_units < 1) max_units = 1;
    int budget = max_units;

    while (c->layer < NN_NUM_LAYERS && budget > 0) {
        const nn_layer_t *L = &k_layers[c->layer];
        int n = (int)units_of(L) - (int)c->unit;
        if (n > budget) n = budget;

        g_layer_kernels[c->layer](L, buf_of(c, L->in_buf), buf_of(c, L->out_buf),
                                 c->acc, c->unit, n);

        c->unit += n;
        budget -= n;
        if (c->unit >= units_of(L)) {  // layer finished, move to the next
            c->layer++;
            c->unit = 0;
        }
    }
    return c->layer >= NN_NUM_LAYERS;
}

int nn_run(nn_ctx_t *c, const int8_t *in) {
    if (nn_begin(c, in) != 0) return -1;
    while (!nn_step(c, (int)nn_total_units())) {
    }
    return nn_argmax(c);
}

const int8_t *nn_scores(const nn_ctx_t *c) {
    return NN_OUT_BUF ? c->buf1 : c->buf0;
}

int nn_argmax(const nn_ctx_t *c) {
    // Softmax is monotonic, so the largest logit is the largest probability --
    // the label is the same and softmax never has to run.
    const int8_t *s = nn_scores(c);
    int best = 0;
    for (int i = 1; i < NN_OUT_LEN; i++) {
        if (s[i] > s[best]) best = i;
    }
    return best;
}

int nn_in_progress(const nn_ctx_t *c) {
    return c->layer < NN_NUM_LAYERS;
}

void nn_abandon(nn_ctx_t *c) {
    if (c == 0) return;
    c->layer = NN_NUM_LAYERS;   // the same thing nn_step() does when it finishes
    c->unit  = 0;
}

static uint32_t layer_in_bytes(const nn_layer_t *L) {
    return (L->op == NN_OP_FC) ? (uint32_t)L->in_c
                                  : (uint32_t)L->in_h * L->in_w * L->in_c;
}

#if NN_DATAFLOW == NN_DATAFLOW_INPUT
// Output rows this layer has finished, given that units [0, unit) are consumed.
//
// Conv row oy needs input rows oy..oy+k-1, so it lands once k complete input
// rows have gone by. Pool consumes k input rows per output row. Dense finishes
// one neuron per unit.
static uint32_t out_rows_done(const nn_layer_t *L, uint32_t unit) {
    if (L->op == NN_OP_FC) return unit;
    const uint32_t in_rows = unit / L->in_w;      // complete input rows
    if (L->op == NN_OP_POOL) return in_rows / L->k;
    return (in_rows >= (uint32_t)L->k) ? in_rows - L->k + 1 : 0;
}
#endif

// Bytes of output finished so far.
static uint32_t out_bytes(const nn_layer_t *L, uint32_t unit) {
#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    // A unit IS an output element, written in order, so the finished region is
    // simply the first `unit` bytes. Dense behaves the same way.
    (void)L; return unit;
#else
    const uint32_t rows = out_rows_done(L, unit);
    return (L->op == NN_OP_FC) ? rows
                               : rows * (uint32_t)L->out_w * L->out_c;
#endif
}

// Input the layer has finished with, and therefore need not carry.
//
// Two derivations, and -- since the pixel-exact frontier landed -- two
// different answers. They are written as separate semantic branches on
// purpose: NN_LIVE_ALIGN is shared, and the exporter's NN_LIVE_MAX was
// computed against the output-stationary rule using it, so weakening the macro
// to 1 would silently change a policy this branch does not own.
//
// OUTPUT-STATIONARY -- UNCHANGED. A unit is one output element. Element `unit`
// sits on output row unit/(out_w*out_c); that row is the first input row still
// needed (conv, stride 1) or row*k (pool, stride k). The position says nothing
// finer than a row here, and the result is rounded DOWN to NN_LIVE_ALIGN:
// keeping a few extra rows costs almost nothing, handing the backend an offset
// it cannot program costs a failed save, and the rounded pointer stays as
// aligned as the buffer base so the MRAM backend keeps off its bounce path.
//
// INPUT-STATIONARY -- PIXEL-EXACT. A unit IS one input pixel, and the defining
// property of the dataflow is that a consumed pixel is dead the moment it has
// been scattered into every output it feeds. So the frontier is exact:
//
//     F = unit * in_c
//
// "Pixel" includes all of that pixel's input channels, which is what in_c is
// doing there. Neither rounding survives. Retiring whole rows held on to up to
// in_w - 1 pixels already scattered; rounding F down to 16 would hold up to 15
// further bytes. Both merely describe the over-retention more precisely -- this
// omits it.
//
// The consequence is deliberate: F is not generally a multiple of
// NN_LIVE_ALIGN, so nn_live() reports an UNALIGNED input pointer in this mode.
// The serializer already copes -- ckpt.c writes at aligned slot offsets and
// runs any partial final block through a zero-filled bounce buffer, so the
// padding carries zeros rather than retired input, and restore copies the
// suffix back to this same input_buffer + F.
//
// Dense is excluded above and keeps the whole-vector rule in both modes.
static uint32_t dead_in_bytes(const nn_layer_t *L, uint32_t unit) {
    if (L->op == NN_OP_FC) return 0;   // every neuron reads the whole vector

#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    const uint32_t oy = unit / ((uint32_t)L->out_w * L->out_c);
    uint32_t row = (L->op == NN_OP_POOL) ? oy * L->k : oy;
    if (row > L->in_h) row = L->in_h;
    const uint32_t bytes = row * L->in_w * L->in_c;
    return bytes & ~(uint32_t)(NN_LIVE_ALIGN - 1);
#else
    // Clamp in PIXELS, not rows: the phantom position unit == in_h*in_w that
    // the exporter's conservative walk visits must retire the whole buffer and
    // no more. nn_position_valid() rejects it at run time; the clamp is here so
    // that the arithmetic is total for any caller that asks anyway.
    const uint32_t total_px = (uint32_t)L->in_h * L->in_w;
    const uint32_t px = (unit > total_px) ? total_px : unit;
    return px * (uint32_t)L->in_c;
#endif
}

void nn_live(nn_ctx_t *c, nn_live_t *lv) {
    if (c->layer >= NN_NUM_LAYERS) {  // finished: only the result matters
        lv->in = NN_OUT_BUF ? c->buf1 : c->buf0;
        lv->in_len = NN_OUT_LEN;
        lv->out = 0;
        lv->out_len = 0;
        lv->acc = 0;
        lv->acc_len = 0;
        return;
    }
    const nn_layer_t *L = &k_layers[c->layer];
    const uint32_t dead = dead_in_bytes(L, c->unit);

    lv->in = buf_of(c, L->in_buf) + dead;
    lv->in_len = layer_in_bytes(L) - dead;
    lv->out = buf_of(c, L->out_buf);
    lv->out_len = out_bytes(L, c->unit);

    // The open accumulators. Nothing is open before the first unit of a layer
    // -- each row is initialised from the bias as it is first touched -- so a
    // checkpoint taken at a layer boundary carries none of this.
    lv->acc = c->acc;
    lv->acc_len = (c->unit > 0) ? acc_len_of(L) * (uint32_t)sizeof(int32_t) : 0;
}

uint32_t nn_live_bytes(nn_ctx_t *c) {
    nn_live_t lv;
    nn_live(c, &lv);
    return lv.in_len + lv.out_len + lv.acc_len;
}

uint32_t nn_total_units(void) {
    uint32_t t = 0;
    for (int i = 0; i < NN_NUM_LAYERS; i++) t += units_of(&k_layers[i]);
    return t;
}

uint32_t nn_units_done(const nn_ctx_t *c) {
    uint32_t t = 0;
    for (int i = 0; i < c->layer && i < NN_NUM_LAYERS; i++) t += units_of(&k_layers[i]);
    return t + ((c->layer < NN_NUM_LAYERS) ? c->unit : 0);
}
