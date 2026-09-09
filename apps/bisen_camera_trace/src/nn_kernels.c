// nn_kernels.c -- the ops, and the registry naming them. Add operations here.
//
// Nothing in this file knows about the descriptor table, the context, or
// resuming. A kernel gets one layer descriptor, an input buffer, an output
// buffer, an accumulator band and a range of units, and consumes that range.
//
// DATAFLOW: both are here, chosen by NN_DATAFLOW (see nn_engine.h). The notes
// below describe the input-stationary branch, which is the BISen arrangement.
//
// Each input pixel is read once and scattered into every output it contributes
// to, rather than each output gathering the inputs it needs. Two consequences,
// and they are the whole reason for it:
//
//   * A unit is one INPUT pixel, so the resume position is an input position.
//     2318 points for this LeNet -- FEWER than output-stationary's 8094, not
//     more. Input-stationary does not buy granularity here; what it buys is
//     the discard below. (Granularity is set by how finely a unit is defined,
//     which is independent of the dataflow -- both modes could go finer.)
//   * An input pixel is dead the moment it has been scattered, so a checkpoint
//     never carries input the resume will not read. (Output-stationary can
//     discard consumed input too; see nn_live. The difference is granularity.)
//
// The cost is that many outputs are part-accumulated at once, so the partial
// sums become live state that a checkpoint has to carry: `acc`, a ring of k
// output rows. That is the trade -- finer stop points, bought with a bigger
// checkpoint. BISen pays for it by storing those partials approximately.
//
// Dense stays output-stationary. Every neuron reads the whole input vector, so
// input-stationary would leave the entire output partial while making nothing
// dead -- strictly worse, and it is why BISen's selective store treats dense
// separately too.
//
// Tensors are NHWC like TFLite, so the exported weights apply unchanged. The
// arithmetic is bit-identical to the gather order: the same int32 products are
// summed, and int32 addition does not care in which order.
#include "nn_kernels.h"
#include "nn_quant.h"

#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT

// ---------------------------------------------------------------------------
// OUTPUT-STATIONARY. One unit = one output element.
//
// Each output's dot product runs to completion and is written once, so at any
// stop point NOTHING is part-summed -- `acc` is untouched and a checkpoint
// carries only the input still to be read plus the output written so far.
// That is the arrangement HAWAII describes for CPU-based layers, and it is why
// they can say partial progress "needs not to be reverted": there isn't any.
//
// A unit is an element rather than a row purely so the engine can stop more
// often. It costs nothing: the index still fits the same uint16, and the
// checkpoint is the same size. 271 stop points become 8094.
// ---------------------------------------------------------------------------

// Square kernel, valid padding, stride 1. One unit = one output element.
static void k_conv(const nn_layer_t *L, const int8_t *in, int8_t *out,
                   int32_t *acc, int u0, int n) {
    const int32_t in_offset = -L->in_zp;
    const int k = L->k, in_c = L->in_c, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w;
    (void)acc;

    // Decode the starting element once, then walk. Units are consecutive, so
    // an odometer is enough -- a divide per element would cost more than the
    // MACs in conv1, where the dot product is only 25 long.
    int oc = u0 % out_c;
    int ox = (u0 / out_c) % out_w;
    int oy = u0 / (out_c * out_w);

    for (int u = u0; u < u0 + n; u++) {
        const int8_t *wp = L->w + oc * k * k * in_c;
        int32_t sum = L->b[oc];
        for (int ky = 0; ky < k; ky++) {
            const int8_t *ip = in + ((oy + ky) * in_w + ox) * in_c;
            for (int kx = 0; kx < k * in_c; kx++) {
                sum += (int32_t)(*wp++) * ((int32_t)ip[kx] + in_offset);
            }
        }
        sum = requantize_conv(sum, L->mult[oc], L->shift[oc]) + L->out_zp;
        out[u] = clamp_i8(sum, L->act_min, 127);   // u IS the output index

        if (++oc == out_c) { oc = 0; if (++ox == out_w) { ox = 0; oy++; } }
    }
}

// Average pooling, stride == pool size. One unit = one output element.
static void k_pool(const nn_layer_t *L, const int8_t *in, int8_t *out,
                   int32_t *acc, int u0, int n) {
    const int k = L->k, c = L->in_c;
    const int in_w = L->in_w, out_w = L->out_w;
    const int32_t count = k * k;
    (void)acc;

    int ch = u0 % c;
    int ox = (u0 / c) % out_w;
    int oy = u0 / (c * out_w);

    for (int u = u0; u < u0 + n; u++) {
        int32_t sum = 0;
        for (int ky = 0; ky < k; ky++) {
            for (int kx = 0; kx < k; kx++) {
                sum += in[((oy * k + ky) * in_w + (ox * k + kx)) * c + ch];
            }
        }
        sum = (sum > 0) ? (sum + count / 2) / count
                        : (sum - count / 2) / count;
        out[u] = clamp_i8(sum, -128, 127);

        if (++ch == c) { ch = 0; if (++ox == out_w) { ox = 0; oy++; } }
    }
}

#else   /* NN_DATAFLOW == NN_DATAFLOW_INPUT */

// An output row is final once the last input row feeding it has been consumed.
// Requantize it into the output buffer; its accumulator slot is then free.
static void flush_conv_row(const nn_layer_t *L, int8_t *out, const int32_t *arow,
                           int oy) {
    const int out_w = L->out_w, out_c = L->out_c;
    for (int ox = 0; ox < out_w; ox++) {
        for (int oc = 0; oc < out_c; oc++) {
            int32_t v = requantize_conv(arow[ox * out_c + oc], L->mult[oc],
                                        L->shift[oc]) + L->out_zp;
            out[(oy * out_w + ox) * out_c + oc] = clamp_i8(v, L->act_min, 127);
        }
    }
}

// Square kernel, valid padding, stride 1. One unit = one input pixel.
static void k_conv(const nn_layer_t *L, const int8_t *in, int8_t *out,
                   int32_t *acc, int u0, int n) {
    const int32_t in_offset = -L->in_zp;
    const int k = L->k, in_c = L->in_c, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int row_len = out_w * out_c;   // accumulators in one output row

    for (int p = u0; p < u0 + n; p++) {
        const int iy = p / in_w, ix = p - iy * in_w;

        // Opening a row. Input row iy is the first to touch output row iy
        // (kernel offset 0), so that is where its accumulators start from the
        // bias. Slot iy%k was vacated when row iy-k flushed, one input row ago.
        if (ix == 0 && iy < out_h) {
            int32_t *arow = acc + (iy % k) * row_len;
            for (int ox = 0; ox < out_w; ox++) {
                for (int oc = 0; oc < out_c; oc++) arow[ox * out_c + oc] = L->b[oc];
            }
        }

        // Scatter. This pixel feeds output rows [iy-k+1, iy] and columns
        // [ix-k+1, ix], clipped to the output -- valid padding, so the border
        // pixels simply feed fewer outputs.
        int oy_lo = iy - k + 1, oy_hi = iy;
        int ox_lo = ix - k + 1, ox_hi = ix;
        if (oy_lo < 0) oy_lo = 0;
        if (oy_hi > out_h - 1) oy_hi = out_h - 1;
        if (ox_lo < 0) ox_lo = 0;
        if (ox_hi > out_w - 1) ox_hi = out_w - 1;

        const int8_t *ip = in + (p * in_c);

        for (int oy = oy_lo; oy <= oy_hi; oy++) {
            const int ky = iy - oy;
            int32_t *arow = acc + (oy % k) * row_len;
            for (int ox = ox_lo; ox <= ox_hi; ox++) {
                const int kx = ix - ox;
                // Weights are [oc][ky][kx][ic].
                const int8_t *wp = L->w + ((ky * k) + kx) * in_c;
                int32_t *ap = arow + ox * out_c;
                for (int oc = 0; oc < out_c; oc++) {
                    const int8_t *w = wp + oc * k * k * in_c;
                    int32_t s = 0;
                    for (int c = 0; c < in_c; c++) {
                        s += (int32_t)w[c] * ((int32_t)ip[c] + in_offset);
                    }
                    ap[oc] += s;
                }
            }
        }

        // Finishing a row. Output row iy-k+1 has now seen all k of its input
        // rows, so it is final.
        if (ix == in_w - 1) {
            const int oy = iy - k + 1;
            if (oy >= 0 && oy < out_h) {
                flush_conv_row(L, out, acc + (oy % k) * row_len, oy);
            }
        }
    }
}

// Average pooling, stride == pool size. Input and output share a scale and zero
// point, so no rescale is needed -- just the rounded mean, the way TFLite does
// it. One unit = one input pixel.
//
// The windows do not overlap, so exactly one output row is ever open and the
// band is a single row.
static void k_pool(const nn_layer_t *L, const int8_t *in, int8_t *out,
                   int32_t *acc, int u0, int n) {
    const int k = L->k, c = L->in_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int32_t count = k * k;

    for (int p = u0; p < u0 + n; p++) {
        const int iy = p / in_w, ix = p - iy * in_w;
        const int oy = iy / k, ox = ix / k;
        if (oy >= out_h) continue;              // ragged tail, if any

        if (ix == 0 && (iy % k) == 0) {         // opening this output row
            for (int i = 0; i < out_w * c; i++) acc[i] = 0;
        }

        if (ox < out_w) {
            const int8_t *ip = in + p * c;
            int32_t *ap = acc + ox * c;
            for (int ch = 0; ch < c; ch++) ap[ch] += ip[ch];
        }

        if (ix == in_w - 1 && (iy % k) == k - 1) {   // row complete
            for (int o = 0; o < out_w; o++) {
                for (int ch = 0; ch < c; ch++) {
                    int32_t a = acc[o * c + ch];
                    a = (a > 0) ? (a + count / 2) / count : (a - count / 2) / count;
                    out[(oy * out_w + o) * c + ch] = clamp_i8(a, -128, 127);
                }
            }
        }
    }
}

#endif  /* NN_DATAFLOW */

// Fully connected. One unit = one output neuron. Output-stationary: see the
// note at the top of this file. Takes `acc` for signature uniformity only.
static void k_fc(const nn_layer_t *L, const int8_t *in, int8_t *out,
                 int32_t *acc, int u0, int n) {
    const int32_t in_offset = -L->in_zp;
    const int n_in = L->in_c;
    (void)acc;

    for (int u = u0; u < u0 + n; u++) {
        const int8_t *wp = L->w + (int32_t)u * n_in;
        int32_t sum = L->b[u];
        for (int i = 0; i < n_in; i++) {
            sum += (int32_t)wp[i] * ((int32_t)in[i] + in_offset);
        }
        sum = requantize_fc(sum, L->mult[u], L->shift[u]) + L->out_zp;
        out[u] = clamp_i8(sum, L->act_min, 127);
    }
}

// Indexed by op id. A new operation is a new kernel above and a new row here.
const nn_kernel_fn nn_kernels[NN_OP_COUNT] = {
    k_conv,  // NN_OP_CONV
    k_pool,  // NN_OP_POOL
    k_fc,    // NN_OP_FC
};
