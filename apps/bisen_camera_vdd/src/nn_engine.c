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

int nn_begin(nn_ctx_t *c, const int8_t *in) {
    // The band has to hold the widest layer's open rows. A generated header
    // states this; refuse loudly rather than scribble past the array.
    if (nn_acc_required() > NN_ACC_LEN) return -1;

    int8_t *dst = buf_of(c, k_layers[0].in_buf);
    for (int i = 0; i < NN_IN_LEN; i++) dst[i] = in[i];
    c->layer = 0;
    c->unit = 0;
    return 0;
}

int nn_step(nn_ctx_t *c, int max_units) {
    if (max_units < 1) max_units = 1;
    int budget = max_units;

    while (c->layer < NN_NUM_LAYERS && budget > 0) {
        const nn_layer_t *L = &k_layers[c->layer];
        int n = (int)units_of(L) - (int)c->unit;
        if (n > budget) n = budget;

        nn_kernels[L->op](L, buf_of(c, L->in_buf), buf_of(c, L->out_buf), c->acc,
                          c->unit, n);

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
// Output-stationary: output element `unit` sits on row unit/(out_w*out_c), and
// that row is the first input row still needed (conv, stride 1) or row*k
// (pool, stride k). Input-stationary: every complete input row behind the
// position, because a pixel is dead the moment it has been scattered.
// Different derivations, same conclusion -- rows before the frontier are gone.
static uint32_t dead_in_bytes(const nn_layer_t *L, uint32_t unit) {
    if (L->op == NN_OP_FC) return 0;   // every neuron reads the whole vector

#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    const uint32_t oy = unit / ((uint32_t)L->out_w * L->out_c);
    uint32_t row = (L->op == NN_OP_POOL) ? oy * L->k : oy;
#else
    uint32_t row = unit / L->in_w;
#endif
    if (row > L->in_h) row = L->in_h;
    uint32_t bytes = row * L->in_w * L->in_c;

    // Round DOWN to the storage granularity. Keeping a few extra rows costs
    // almost nothing; handing the backend an offset it cannot program costs a
    // failed save. Rounding down also leaves the pointer as aligned as the
    // buffer base, which keeps the MRAM backend off its bounce-buffer path.
    return bytes & ~(uint32_t)(NN_LIVE_ALIGN - 1);
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
