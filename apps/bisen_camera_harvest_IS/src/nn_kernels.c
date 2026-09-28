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
//
// SIMD (task 065). With NN_SIMD=1 (the default) the multiply-accumulate loops
// of convolution and dense run on the M4's dual 16-bit MAC -- see nn_simd.h for
// the instructions and for why the sums are exactly the scalar loop's -- and
// the loops around them are rewritten to keep values in registers. What is
// deliberately NOT changed:
//
//   * The unit, the loop order between units, and what is written where. A
//     call over units [u0, u0+n) writes exactly the context bytes the scalar
//     kernel writes, with the same values, so the context after every nn_step
//     is byte-identical and a checkpoint cannot tell the two builds apart.
//     Output-stationary still finishes each dot product in a register before
//     writing it; it now runs up to three output channels of one pixel at a
//     time over the pixel's window copied into one contiguous stack buffer,
//     so each input word is loaded and widened once per three channels.
//     Input-stationary still scatters one input pixel into every output it
//     feeds and read-modify-writes the band exactly as before; the dot product
//     over the pixel's channels is dual-MAC, and a single-channel layer reads
//     its weights from a per-call reordered stack copy so one output row's
//     targets are one contiguous run.
//   * Requantization. Same functions from nn_quant.h, same arguments. (After
//     this change it is about a fifth of the convolution time.)
//   * Pooling arithmetic. The same sums and the same rounding expression,
//     walked with pointers and odometers instead of per-element index
//     arithmetic and divides.
//
// Every scalar loop of the frozen engine is still here, unchanged, and is what
// NN_SIMD=0 builds. It is also the fallback for a descriptor the dual-lane path
// cannot hold exactly (nn_simd_offset_ok) or an input-stationary layer wider
// than NN_SIMD_IS_CMAX channels. Nothing here keeps state between calls: the
// stack buffers are rebuilt on every call and never reach the context.
#include "nn_kernels.h"
#include "nn_quant.h"
#include "nn_simd.h"

// One predicate for diagnostics AND int8 optimized dispatch. All scalar
// fallback reasons are public, including packed storage and undersized CMAX.
static nn_path_t path_for(const nn_layer_t *L) {
    if (L->op >= NN_OP_COUNT) return NN_PATH_INVALID;
    if (L->op == NN_OP_POOL)
        return NN_SIMD ? NN_PATH_POOL_OPTIMIZED : NN_PATH_POOL_SCALAR;
    if (NN_WO_LAYER_PACKED(L)) return NN_PATH_PACKED_SCALAR;
    if (!NN_SIMD) return NN_PATH_SCALAR_DISABLED;
    if (!nn_simd_offset_ok(-(int32_t)L->in_zp)) return NN_PATH_SCALAR_OFFSET;
#if NN_DATAFLOW == NN_DATAFLOW_INPUT
    if (L->op == NN_OP_CONV && L->in_c > NN_SIMD_IS_CMAX)
        return NN_PATH_SCALAR_CMAX;
#endif
    return NN_PATH_INT8_SIMD;
}


const char *nn_path_name(nn_path_t path) {
    switch (path) {
    case NN_PATH_PACKED_SCALAR: return "packed-scalar (fused low-bit SIMD not implemented)";
    case NN_PATH_SCALAR_DISABLED: return "int8-scalar (NN_SIMD=0)";
    case NN_PATH_SCALAR_OFFSET: return "int8-scalar (offset exceeds SIMD lanes)";
    case NN_PATH_SCALAR_CMAX: return "int8-scalar (input channels exceed CMAX)";
    case NN_PATH_INT8_SIMD: return "int8-065-optimized";
    case NN_PATH_POOL_SCALAR: return "pool-scalar";
    case NN_PATH_POOL_OPTIMIZED: return "pool-optimized-scalar (no DSP MAC)";
    default: return "invalid-layer";
    }
}

uint32_t nn_simd_is_cmax(void) { return NN_SIMD_IS_CMAX; }
const char *nn_simd_implementation(void) {
    return NN_SIMD ? NN_SIMD_IMPL : "disabled";
}

#if NN_SIMD
// ---------------------------------------------------------------------------
// The shared inner loop: G output channels (1, 2 or 3) over one contiguous run
// of `len` inputs. Channel j's weights for the run start at w + j*ws. Four
// inputs per step: one load, widened once with the offset added, then two
// SMLADs per channel. A run that is not a multiple of four finishes with the
// scalar expression.
//
// Why at most three channels: on the M4 with this compiler (arm-none-eabi-gcc
// 10.3 -O3), three channels compile to a 21-instruction loop for 12 MACs with
// every value in a register -- one weight pointer, the other two channels
// reached through register-offset addressing. Every four-channel form tried
// spilled pointers or accumulators to the stack inside the loop (29 to 33
// instructions for 16 MACs); see RESULT-065-simd.md.
//
// G is a constant at every call site; always_inline lets the compiler drop the
// unused channels and keep a[] in registers.
NN_SIMD_INLINE void dot_run(const int G, const int8_t *x, const int8_t *w,
                            int ws, int len, uint32_t off2, int32_t off,
                            int32_t *a) {
    int32_t a0 = a[0];
    int32_t a1 = (G > 1) ? a[1] : 0;
    int32_t a2 = (G > 2) ? a[2] : 0;
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        const uint32_t xv = nn_rd32(x + i);
        const uint32_t xe = nn_sxtab16(off2, xv);        // x0+off, x2+off
        const uint32_t xd = nn_sxtab16_ror8(off2, xv);   // x1+off, x3+off
        uint32_t wv = nn_rd32(w + i);                    // w0 w1 w2 w3
        a0 = nn_smlad(nn_sxtb16(wv), xe, a0);            // + w0*x0 + w2*x2
        a0 = nn_smlad(nn_sxtb16_ror8(wv), xd, a0);       // + w1*x1 + w3*x3
        if (G > 1) {
            wv = nn_rd32(w + ws + i);
            a1 = nn_smlad(nn_sxtb16(wv), xe, a1);
            a1 = nn_smlad(nn_sxtb16_ror8(wv), xd, a1);
        }
        if (G > 2) {
            wv = nn_rd32(w + 2 * ws + i);
            a2 = nn_smlad(nn_sxtb16(wv), xe, a2);
            a2 = nn_smlad(nn_sxtb16_ror8(wv), xd, a2);
        }
    }
    // At most three left: straight-line code rather than a loop, so the
    // compiler does not generate a general loop for a trip count below four.
    for (int t = 0; t < 3; t++, i++) {
        if (i >= len) break;
        const int32_t xi = (int32_t)x[i] + off;
        a0 = nn_add(a0, (int32_t)w[i] * xi);
        if (G > 1) a1 = nn_add(a1, (int32_t)w[ws + i] * xi);
        if (G > 2) a2 = nn_add(a2, (int32_t)w[2 * ws + i] * xi);
    }
    a[0] = a0;
    if (G > 1) a[1] = a1;
    if (G > 2) a[2] = a2;
}
#endif  /* NN_SIMD */

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

#if NN_SIMD
// Largest window (k*k*in_c bytes) copied into one contiguous run on the stack
// before its dot products. A larger window takes the row-by-row path below,
// which needs no buffer; -DNN_SIMD_PATCH_MAX=0 forces that path everywhere
// (the host qualification builds it that way too, so both paths are tested).
#ifndef NN_SIMD_PATCH_MAX
#define NN_SIMD_PATCH_MAX 256
#endif

// Requantize one finished conv sum and write it: the frozen kernel's two lines.
NN_SIMD_INLINE void put_conv(const nn_layer_t *L, int8_t *op, int oc, int32_t sum) {
    sum = requantize_conv(sum, L->mult[oc], L->shift[oc]) + L->out_zp;
    op[oc] = clamp_i8(sum, L->act_min, 127);
}

// G channels of one output pixel whose window has been gathered into `patch`:
// P = k*k*in_c bytes in [ky][kx][ic] order, the same order as one output
// channel's weights, so each channel is ONE dot product of length P.
NN_SIMD_INLINE void conv_os_group_p(const int G, const nn_layer_t *L,
                                    const int8_t *patch, int P, int8_t *op,
                                    int oc, uint32_t off2, int32_t off) {
    int32_t a[3];
    for (int j = 0; j < G; j++) a[j] = L->b[oc + j];
    dot_run(G, patch, L->w + oc * P, P, P, off2, off, a);
    for (int j = 0; j < G; j++) put_conv(L, op, oc + j, a[j]);
}

// The same without the copy: the window is k rows of k*in_c bytes, each row
// contiguous in the NHWC input and in the weights, so each row is a dot_run.
NN_SIMD_INLINE void conv_os_group_rows(const int G, const nn_layer_t *L,
                                       const int8_t *ip, int8_t *op, int oc,
                                       uint32_t off2, int32_t off) {
    const int k = L->k;
    const int row = k * L->in_c;           // bytes per kernel row
    const int ws = k * row;                // weights per output channel
    const int in_stride = L->in_w * L->in_c;
    const int8_t *wp = L->w + oc * ws;
    int32_t a[3];
    for (int j = 0; j < G; j++) a[j] = L->b[oc + j];
    for (int ky = 0; ky < k; ky++) {
        dot_run(G, ip + ky * in_stride, wp + ky * row, ws, row, off2, off, a);
    }
    for (int j = 0; j < G; j++) put_conv(L, op, oc + j, a[j]);
}

// Copy the k rows of one window, `row` bytes each and in_stride apart, into
// one contiguous run. A private buffer on the stack; nothing in the context is
// written, so the state a checkpoint sees is unaffected.
NN_SIMD_INLINE void gather_window(int8_t *dst, const int8_t *src, int in_stride,
                                  int row, int rows) {
    for (int r = 0; r < rows; r++) {
        const int8_t *s = src + r * in_stride;  // indexed: never past the last row
        int8_t *d = dst + r * row;
        int i = 0;
        for (; i + 4 <= row; i += 4) {
            uint32_t v = nn_rd32(s + i);
            NN_SIMD_OPAQUE(v);                  // keep it a load/store pair
            memcpy(d + i, &v, sizeof v);
        }
        for (int t = 0; t < 3; t++, i++) {      // at most three bytes left
            if (i >= row) break;
            int8_t b = s[i];
            NN_SIMD_OPAQUE(b);
            d[i] = b;
        }
    }
}

// Units [u0, u0+n) are output elements in (oy, ox, oc) order, oc fastest, so
// the range is a run of whole or partial pixels. Each pixel's window is
// gathered once, then its channels in the range go through in groups of 3 and
// one group of 2 or 1. Each unit u is written to out[u] exactly once, with the
// value the scalar kernel writes.
static void conv_os_simd(const nn_layer_t *L, const int8_t *in, int8_t *out,
                         int u0, int n, int32_t off) {
    const int k = L->k, out_c = L->out_c, out_w = L->out_w, in_c = L->in_c;
    const int row = k * in_c, P = k * row;
    const int in_stride = L->in_w * in_c;
    const int gathered = P <= NN_SIMD_PATCH_MAX;
    const uint32_t off2 = nn_lanes2(off);
    int8_t patch[NN_SIMD_PATCH_MAX > 0 ? NN_SIMD_PATCH_MAX : 4]
        __attribute__((aligned(4)));
    int oc = u0 % out_c;
    int ox = (u0 / out_c) % out_w;
    int oy = u0 / (out_c * out_w);
    int left = n;
    while (left > 0) {
        int end = out_c;
        if (end - oc > left) end = oc + left;
        left -= end - oc;
        const int8_t *ip = in + (oy * L->in_w + ox) * in_c;
        int8_t *op = out + (oy * out_w + ox) * out_c;
        if (gathered) {
            gather_window(patch, ip, in_stride, row, k);
            while (end - oc >= 3) { conv_os_group_p(3, L, patch, P, op, oc, off2, off); oc += 3; }
            if (end - oc == 2) conv_os_group_p(2, L, patch, P, op, oc, off2, off);
            else if (end - oc == 1) conv_os_group_p(1, L, patch, P, op, oc, off2, off);
        } else {
            while (end - oc >= 3) { conv_os_group_rows(3, L, ip, op, oc, off2, off); oc += 3; }
            if (end - oc == 2) conv_os_group_rows(2, L, ip, op, oc, off2, off);
            else if (end - oc == 1) conv_os_group_rows(1, L, ip, op, oc, off2, off);
        }
        oc = 0;
        if (++ox == out_w) { ox = 0; oy++; }
    }
}
#endif  /* NN_SIMD */

// Packed scalar twin retained from 063.
static void k_conv_wo(const nn_layer_t *L, const int8_t *in, int8_t *out,
                      int u0, int n) {
    const int32_t in_offset = -L->in_zp;
    const int k = L->k, in_c = L->in_c, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w;
    const uint8_t *wq = (const uint8_t *)L->w;
    const uint32_t bits = L->w_bits;

    int oc = u0 % out_c;
    int ox = (u0 / out_c) % out_w;
    int oy = u0 / (out_c * out_w);

    for (int u = u0; u < u0 + n; u++) {
        uint32_t wi = (uint32_t)oc * k * k * in_c;
        int32_t sum = L->b[oc];
        for (int ky = 0; ky < k; ky++) {
            const int8_t *ip = in + ((oy + ky) * in_w + ox) * in_c;
            for (int kx = 0; kx < k * in_c; kx++) {
                sum += nn_wo_raw(wq, bits, wi++) * ((int32_t)ip[kx] + in_offset);
            }
        }
        sum = requantize_conv(sum, L->mult[oc], L->shift[oc]) + L->out_zp;
        out[u] = clamp_i8(sum, L->act_min, 127);

        if (++oc == out_c) { oc = 0; if (++ox == out_w) { ox = 0; oy++; } }
    }
}

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

#if NN_SIMD
// The scalar pool kernel below, walked with pointers instead of recomputing
// ((oy*k + ky)*in_w + ox*k + kx)*c + ch for every input, and with the 2x2 case
// written out so its divisor is the constant 4 -- the same expression as
// (sum +- count/2) / count with count = k*k = 4, C's truncating division
// either way. Same units, same values, same single write per unit.
static void pool_os_fast(const nn_layer_t *L, const int8_t *in, int8_t *out,
                         int u0, int n) {
    const int k = L->k, c = L->in_c;
    const int in_w = L->in_w, out_w = L->out_w;
    const int32_t count = k * k;
    const int rstride = in_w * c;          // one input row
    int ch = u0 % c;
    int ox = (u0 / c) % out_w;
    int oy = u0 / (c * out_w);
    const int8_t *win = in + ((oy * k) * in_w + ox * k) * c;   // window, channel 0
    for (int u = u0; u < u0 + n; u++) {
        const int8_t *p = win + ch;
        int32_t sum;
        if (k == 2) {
            sum = (int32_t)p[0] + p[c] + p[rstride] + p[rstride + c];
            sum = (sum > 0) ? (sum + 2) / 4 : (sum - 2) / 4;
        } else {
            sum = 0;
            for (int ky = 0; ky < k; ky++) {
                for (int kx = 0; kx < k; kx++) sum += p[ky * rstride + kx * c];
            }
            sum = (sum > 0) ? (sum + count / 2) / count
                            : (sum - count / 2) / count;
        }
        out[u] = clamp_i8(sum, -128, 127);

        if (++ch == c) {
            ch = 0;
            win += k * c;
            if (++ox == out_w) { ox = 0; oy++; win = in + (oy * k) * rstride; }
        }
    }
}
#endif  /* NN_SIMD */

// Average pooling, stride == pool size. One unit = one output element.
static void k_pool(const nn_layer_t *L, const int8_t *in, int8_t *out,
                   int32_t *acc, int u0, int n) {
    const int k = L->k, c = L->in_c;
    const int in_w = L->in_w, out_w = L->out_w;
    const int32_t count = k * k;
    (void)acc;

#if NN_SIMD
    pool_os_fast(L, in, out, u0, n);
    return;
#endif

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

#if NN_SIMD
// Capacity is selected and range-checked in nn_engine.h, after the model header.

// flush_conv_row above, walking both rows with pointers: the same requantize
// call on the same accumulator for every output, written in the same order.
static void flush_conv_row_fast(const nn_layer_t *L, int8_t *out,
                                const int32_t *arow, int oy) {
    const int out_w = L->out_w, out_c = L->out_c;
    const int32_t *mult = L->mult, *shift = L->shift;
    const int32_t out_zp = L->out_zp, act_min = L->act_min;
    int8_t *o = out + oy * out_w * out_c;
    for (int ox = 0; ox < out_w; ox++) {
        for (int oc = 0; oc < out_c; oc++) {
            const int32_t v = requantize_conv(*arow++, mult[oc], shift[oc]) + out_zp;
            *o++ = clamp_i8(v, act_min, 127);
        }
    }
}

// G output channels (1..3) at one scatter target: the pixel's in_c channels,
// pre-widened (xe/xd per four channels, then the last TAIL = in_c % 4 as
// xt0..xt2), against G weight slices ws apart. Each finished sum is added to
// its accumulator once, as the scalar kernel's `ap[oc] += s`. TAIL is a
// constant at every call site, so the tail is straight-line code.
NN_SIMD_INLINE void is_group(const int G, const int TAIL, const int8_t *w,
                             int ws, const uint32_t *xe, const uint32_t *xd,
                             int groups, int32_t xt0, int32_t xt1, int32_t xt2,
                             int32_t *ap) {
    int32_t s0 = 0, s1 = 0, s2 = 0;
    // Not unrolled: the trip count is a run-time in_c/4, and letting GCC peel
    // it up to the array bound multiplied this function's size ten-fold.
#pragma GCC unroll 1
    for (int g = 0; g < groups; g++, w += 4) {
        const uint32_t e = xe[g], d = xd[g];
        uint32_t wv = nn_rd32(w);
        s0 = nn_smlad(nn_sxtb16(wv), e, s0);
        s0 = nn_smlad(nn_sxtb16_ror8(wv), d, s0);
        if (G > 1) {
            wv = nn_rd32(w + ws);
            s1 = nn_smlad(nn_sxtb16(wv), e, s1);
            s1 = nn_smlad(nn_sxtb16_ror8(wv), d, s1);
        }
        if (G > 2) {
            wv = nn_rd32(w + 2 * ws);
            s2 = nn_smlad(nn_sxtb16(wv), e, s2);
            s2 = nn_smlad(nn_sxtb16_ror8(wv), d, s2);
        }
    }
    // w now points at the tail of the slice
    if (TAIL > 0) {
        s0 = nn_add(s0, (int32_t)w[0] * xt0);
        if (G > 1) s1 = nn_add(s1, (int32_t)w[ws] * xt0);
        if (G > 2) s2 = nn_add(s2, (int32_t)w[2 * ws] * xt0);
    }
    if (TAIL > 1) {
        s0 = nn_add(s0, (int32_t)w[1] * xt1);
        if (G > 1) s1 = nn_add(s1, (int32_t)w[ws + 1] * xt1);
        if (G > 2) s2 = nn_add(s2, (int32_t)w[2 * ws + 1] * xt1);
    }
    if (TAIL > 2) {
        s0 = nn_add(s0, (int32_t)w[2] * xt2);
        if (G > 1) s1 = nn_add(s1, (int32_t)w[ws + 2] * xt2);
        if (G > 2) s2 = nn_add(s2, (int32_t)w[2 * ws + 2] * xt2);
    }
    ap[0] = nn_add(ap[0], s0);
    if (G > 1) ap[1] = nn_add(ap[1], s1);
    if (G > 2) ap[2] = nn_add(ap[2], s2);
}

// One output row's scatter targets for this pixel, ox = ox_lo .. ox_lo+nt-1.
// Target t's accumulators start at ap + t*out_c and its weight slice at
// wp - t*in_c: kx = ix - ox falls by one as ox rises by one. (Indexed rather
// than stepped, here and below, so no pointer is ever formed outside the
// weight table or the band -- a stepped pointer would run one step past.)
NN_SIMD_INLINE void is_row(const int TAIL, int32_t *ap, const int8_t *wp,
                           int nt, int out_c, int ws, int in_c,
                           const uint32_t *xe, const uint32_t *xd, int groups,
                           int32_t xt0, int32_t xt1, int32_t xt2) {
    for (int t = 0; t < nt; t++) {
        int32_t *at = ap + t * out_c;
        const int8_t *wt = wp - t * in_c;
        int oc = 0;
        for (; out_c - oc >= 3; oc += 3) {
            is_group(3, TAIL, wt + oc * ws, ws, xe, xd, groups, xt0, xt1, xt2, at + oc);
        }
        if (out_c - oc == 2) {
            is_group(2, TAIL, wt + oc * ws, ws, xe, xd, groups, xt0, xt1, xt2, at + oc);
        } else if (out_c - oc == 1) {
            is_group(1, TAIL, wt + oc * ws, ws, xe, xd, groups, xt0, xt1, xt2, at + oc);
        }
    }
}

// The last TAIL = in_c % 4 channels of one weight slice (starting at w[4])
// against the pixel's: channels 4 and 5 as one int16 pair when there are two
// or more (xp = the pixel's pair), channel 4 or 6 on its own otherwise.
NN_SIMD_INLINE int32_t is_tail(const int TAIL, const int8_t *w, int32_t s,
                               int32_t xt0, uint32_t xp, int32_t xt2) {
    if (TAIL == 1) s = nn_add(s, (int32_t)w[4] * xt0);
    if (TAIL >= 2) s = nn_smlad(nn_rd_pair(w + 4), xp, s);
    if (TAIL == 3) s = nn_add(s, (int32_t)w[6] * xt2);
    return s;
}

// The same for 4..7 input channels (one group of four plus a tail): the
// pixel is held in registers, and output channels go two at a time.
NN_SIMD_INLINE void is_row_g1(const int TAIL, int32_t *ap, const int8_t *wp,
                              int nt, int out_c, int ws, int in_c, uint32_t e,
                              uint32_t d, int32_t xt0, int32_t xt1, int32_t xt2) {
    const uint32_t xp = nn_pack2(xt0, xt1);  // channels 4, 5 (+offset)
    for (int t = 0; t < nt; t++) {
        int32_t *const at = ap + t * out_c;
        const int8_t *const wt = wp - t * in_c;
        int oc = 0;
        for (; out_c - oc >= 2; oc += 2) {
            const int8_t *w = wt + oc * ws;
            int32_t *a = at + oc;
            const uint32_t v0 = nn_rd32(w), v1 = nn_rd32(w + ws);
            int32_t s0 = nn_smuad(nn_sxtb16(v0), e);
            int32_t s1 = nn_smuad(nn_sxtb16(v1), e);
            s0 = nn_smlad(nn_sxtb16_ror8(v0), d, s0);
            s1 = nn_smlad(nn_sxtb16_ror8(v1), d, s1);
            s0 = is_tail(TAIL, w, s0, xt0, xp, xt2);
            s1 = is_tail(TAIL, w + ws, s1, xt0, xp, xt2);
            a[0] = nn_add(a[0], s0);
            a[1] = nn_add(a[1], s1);
        }
        if (oc < out_c) {
            const int8_t *w = wt + oc * ws;
            const uint32_t v0 = nn_rd32(w);
            int32_t s0 = nn_smuad(nn_sxtb16(v0), e);
            s0 = nn_smlad(nn_sxtb16_ror8(v0), d, s0);
            s0 = is_tail(TAIL, w, s0, xt0, xp, xt2);
            at[oc] = nn_add(at[oc], s0);
        }
    }
}

// The same for a single input channel, where each dot product is one multiply.
NN_SIMD_INLINE void is_row_c1(int32_t *ap, const int8_t *wp, int nt, int out_c,
                              int ws, int32_t xv) {
    for (int t = 0; t < nt; t++) {
        int32_t *const at = ap + t * out_c;
        const int8_t *const wt = wp - t;
        int oc = 0;
        for (; out_c - oc >= 3; oc += 3) {
            const int8_t *w = wt + oc * ws;
            at[oc] = nn_add(at[oc], (int32_t)w[0] * xv);
            at[oc + 1] = nn_add(at[oc + 1], (int32_t)w[ws] * xv);
            at[oc + 2] = nn_add(at[oc + 2], (int32_t)w[2 * ws] * xv);
        }
        for (; oc < out_c; oc++) at[oc] = nn_add(at[oc], (int32_t)wt[oc * ws] * xv);
    }
}

// Single input channel, with the weights reordered once per call (see
// is_c1_table): the targets of one output row, taken in rising ox with their
// channels, are one contiguous run of accumulators AND one contiguous run of
// weights, so the whole row is a flat multiply-add over `len` entries.
NN_SIMD_INLINE void is_run_c1(int32_t *a, const int8_t *r, int len, int32_t xv) {
    int j = 0;
    for (; j + 4 <= len; j += 4) {
        const int32_t a0 = nn_add(a[j], (int32_t)r[j] * xv);
        const int32_t a1 = nn_add(a[j + 1], (int32_t)r[j + 1] * xv);
        const int32_t a2 = nn_add(a[j + 2], (int32_t)r[j + 2] * xv);
        const int32_t a3 = nn_add(a[j + 3], (int32_t)r[j + 3] * xv);
        a[j] = a0; a[j + 1] = a1; a[j + 2] = a2; a[j + 3] = a3;
    }
    if (len & 2) {                           // at most three left
        const int32_t a0 = nn_add(a[j], (int32_t)r[j] * xv);
        const int32_t a1 = nn_add(a[j + 1], (int32_t)r[j + 1] * xv);
        a[j] = a0; a[j + 1] = a1;
        j += 2;
    }
    if (len & 1) a[j] = nn_add(a[j], (int32_t)r[j] * xv);
}

// Largest single-channel weight table (k*k*out_c bytes) the scatter reorders
// on the stack; a bigger layer uses is_row_c1. -DNN_SIMD_IS_C1TAB_MAX=0 forces
// is_row_c1 everywhere (the host qualification builds that way too).
#ifndef NN_SIMD_IS_C1TAB_MAX
#define NN_SIMD_IS_C1TAB_MAX 512
#endif

// R[(ky*k + (k-1-kx))*out_c + oc] = w[oc][ky][kx]: kernel positions with kx
// reversed, all output channels of a position together. Target ox = ox_lo + t
// has kx = kx0 - t, i.e. row ky*k + (k-1-kx0) + t: one row further per target.
static void is_c1_table(const nn_layer_t *L, int8_t *R) {
    const int k = L->k, out_c = L->out_c;
    const int8_t *src = L->w;                // [oc][ky][kx], read in order
    for (int oc = 0; oc < out_c; oc++) {
        for (int ky = 0; ky < k; ky++) {
            int8_t *dst = R + (ky * k + (k - 1)) * out_c + oc;   // kx = 0
            for (int kx = 0; kx < k; kx++, dst -= out_c) *dst = *src++;
        }
    }
}

// Opening an output row: its accumulators start from the channel biases when
// input pixel (iy, 0) arrives -- the scalar kernel's rule, written with a pointer.
NN_SIMD_INLINE void is_open_row(const nn_layer_t *L, int32_t *acc, int iy,
                                int row_len) {
    int32_t *a = acc + (iy % L->k) * row_len;
    for (int ox = 0; ox < L->out_w; ox++) {
        for (int oc = 0; oc < L->out_c; oc++) *a++ = L->b[oc];
    }
}

// Finishing an output row once its last input pixel (iy, in_w-1) is in -- the
// scalar kernel's rule; flush_conv_row_fast writes exactly what it writes.
NN_SIMD_INLINE void is_close_row(const nn_layer_t *L, int8_t *out, int32_t *acc,
                                 int iy, int row_len) {
    const int oy = iy - L->k + 1;
    if (oy >= 0 && oy < L->out_h) {
        flush_conv_row_fast(L, out, acc + (oy % L->k) * row_len, oy);
    }
}

// The output rows input row iy feeds, [oy_lo, oy_hi], and the band slot of
// oy_lo (== oy_lo % k). They change only when iy does.
NN_SIMD_INLINE void is_rows_of(const nn_layer_t *L, int iy, int *oy_lo,
                               int *oy_hi, int *slot_lo) {
    int lo = iy - L->k + 1, hi = iy;
    if (lo < 0) lo = 0;
    if (hi > L->out_h - 1) hi = L->out_h - 1;
    *oy_lo = lo;
    *oy_hi = hi;
    *slot_lo = lo % L->k;
}

// The input-stationary kernel, SIMD form, in three variants chosen once per
// call. All three keep the scalar kernel's unit (one input pixel), its row
// opening and flushing, its scatter bounds, and the set of accumulators each
// pixel read-modify-writes with the value added to each. What differs is only
// how the loops are written: pixel position by odometer instead of a divide,
// the band slot as a ring index instead of oy % k, one output row of targets
// at a time with pointer strides, and the per-target dot product over the
// pixel's channels on the dual MAC.

// Single input channel, weights reordered for this call (is_c1_table): for
// one pixel and one output row, all targets' accumulators and all their
// weights are one contiguous run each.
static void conv_is_c1tab(const nn_layer_t *L, const int8_t *in, int8_t *out,
                          int32_t *acc, int u0, int n, int32_t in_offset) {
    const int k = L->k, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int row_len = out_w * out_c;
    int8_t R[NN_SIMD_IS_C1TAB_MAX > 0 ? NN_SIMD_IS_C1TAB_MAX : 4]
        __attribute__((aligned(4)));
    is_c1_table(L, R);
    int iy = u0 / in_w, ix = u0 - iy * in_w;
    int oy_lo, oy_hi, slot_lo;
    is_rows_of(L, iy, &oy_lo, &oy_hi, &slot_lo);

    for (int p = u0; p < u0 + n; p++) {
        if (ix == 0 && iy < out_h) is_open_row(L, acc, iy, row_len);

        int ox_lo = ix - k + 1, ox_hi = ix;
        if (ox_lo < 0) ox_lo = 0;
        if (ox_hi > out_w - 1) ox_hi = out_w - 1;
        const int len = (ox_hi - ox_lo + 1) * out_c;
        const int32_t xv = (int32_t)in[p] + in_offset;
        // Row oy's targets start at R row (ky*k + k-1 - kx0), kx0 = ix - ox_lo.
        const int r0 = (k - 1) - (ix - ox_lo);
        int32_t *a = acc + ox_lo * out_c;
        int slot = slot_lo;
        for (int oy = oy_lo; oy <= oy_hi; oy++) {
            const int ky = iy - oy;
            is_run_c1(a + slot * row_len, R + (ky * k + r0) * out_c, len, xv);
            if (++slot == k) slot = 0;
        }

        if (ix == in_w - 1) is_close_row(L, out, acc, iy, row_len);
        if (++ix == in_w) {
            ix = 0;
            iy++;
            is_rows_of(L, iy, &oy_lo, &oy_hi, &slot_lo);
        }
    }
}

// Single input channel, straight from the weights (a one-pixel call, or a
// table that would not fit).
static void conv_is_c1(const nn_layer_t *L, const int8_t *in, int8_t *out,
                       int32_t *acc, int u0, int n, int32_t in_offset) {
    const int k = L->k, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int row_len = out_w * out_c;
    const int ws = k * k;                    // in_c == 1
    int iy = u0 / in_w, ix = u0 - iy * in_w;
    int oy_lo, oy_hi, slot_lo;
    is_rows_of(L, iy, &oy_lo, &oy_hi, &slot_lo);

    for (int p = u0; p < u0 + n; p++) {
        if (ix == 0 && iy < out_h) is_open_row(L, acc, iy, row_len);

        int ox_lo = ix - k + 1, ox_hi = ix;
        if (ox_lo < 0) ox_lo = 0;
        if (ox_hi > out_w - 1) ox_hi = out_w - 1;
        const int nt = ox_hi - ox_lo + 1;
        const int32_t xv = (int32_t)in[p] + in_offset;
        int slot = slot_lo;
        for (int oy = oy_lo; oy <= oy_hi; oy++) {
            const int ky = iy - oy;
            is_row_c1(acc + slot * row_len + ox_lo * out_c,
                      L->w + ky * k + (ix - ox_lo), nt, out_c, ws, xv);
            if (++slot == k) slot = 0;
        }

        if (ix == in_w - 1) is_close_row(L, out, acc, iy, row_len);
        if (++ix == in_w) {
            ix = 0;
            iy++;
            is_rows_of(L, iy, &oy_lo, &oy_hi, &slot_lo);
        }
    }
}

// Two or more input channels: the pixel's channels are widened once, with the
// offset added, and every target's dot product runs on the dual MAC.
static void conv_is_cn(const nn_layer_t *L, const int8_t *in, int8_t *out,
                       int32_t *acc, int u0, int n, int32_t in_offset) {
    const int k = L->k, in_c = L->in_c, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int row_len = out_w * out_c;
    const int ws = k * k * in_c;             // weights per output channel
    const uint32_t off2 = nn_lanes2(in_offset);
    const int groups = in_c >> 2, tail = in_c & 3, t0 = in_c & ~3;
    uint32_t xe[(NN_SIMD_IS_CMAX + 3) / 4] = {0}, xd[(NN_SIMD_IS_CMAX + 3) / 4] = {0};
    int iy = u0 / in_w, ix = u0 - iy * in_w;
    int oy_lo, oy_hi, slot_lo;
    is_rows_of(L, iy, &oy_lo, &oy_hi, &slot_lo);

    for (int p = u0; p < u0 + n; p++) {
        if (ix == 0 && iy < out_h) is_open_row(L, acc, iy, row_len);

        int ox_lo = ix - k + 1, ox_hi = ix;
        if (ox_lo < 0) ox_lo = 0;
        if (ox_hi > out_w - 1) ox_hi = out_w - 1;
        const int nt = ox_hi - ox_lo + 1;

        const int8_t *ip = in + (p * in_c);
        for (int g = 0; g < groups; g++) {
            const uint32_t v = nn_rd32(ip + 4 * g);
            xe[g] = nn_sxtab16(off2, v);         // channels 4g, 4g+2 (+offset)
            xd[g] = nn_sxtab16_ror8(off2, v);    // channels 4g+1, 4g+3
        }
        const int32_t xt0 = (tail > 0) ? (int32_t)ip[t0] + in_offset : 0;
        const int32_t xt1 = (tail > 1) ? (int32_t)ip[t0 + 1] + in_offset : 0;
        const int32_t xt2 = (tail > 2) ? (int32_t)ip[t0 + 2] + in_offset : 0;

        int slot = slot_lo;
        for (int oy = oy_lo; oy <= oy_hi; oy++) {
            const int ky = iy - oy;
            // Target (oy, ox_lo): accumulators and weight slice [oc][ky][kx][ic].
            int32_t *ap = acc + slot * row_len + ox_lo * out_c;
            const int8_t *wp = L->w + ((ky * k) + (ix - ox_lo)) * in_c;
            if (groups == 1) {
                switch (tail) {
                case 0: is_row_g1(0, ap, wp, nt, out_c, ws, in_c, xe[0], xd[0], xt0, xt1, xt2); break;
                case 1: is_row_g1(1, ap, wp, nt, out_c, ws, in_c, xe[0], xd[0], xt0, xt1, xt2); break;
                case 2: is_row_g1(2, ap, wp, nt, out_c, ws, in_c, xe[0], xd[0], xt0, xt1, xt2); break;
                default: is_row_g1(3, ap, wp, nt, out_c, ws, in_c, xe[0], xd[0], xt0, xt1, xt2); break;
                }
            } else {
                switch (tail) {
                case 0: is_row(0, ap, wp, nt, out_c, ws, in_c, xe, xd, groups, xt0, xt1, xt2); break;
                case 1: is_row(1, ap, wp, nt, out_c, ws, in_c, xe, xd, groups, xt0, xt1, xt2); break;
                case 2: is_row(2, ap, wp, nt, out_c, ws, in_c, xe, xd, groups, xt0, xt1, xt2); break;
                default: is_row(3, ap, wp, nt, out_c, ws, in_c, xe, xd, groups, xt0, xt1, xt2); break;
                }
            }
            if (++slot == k) slot = 0;
        }

        if (ix == in_w - 1) is_close_row(L, out, acc, iy, row_len);
        if (++ix == in_w) {
            ix = 0;
            iy++;
            is_rows_of(L, iy, &oy_lo, &oy_hi, &slot_lo);
        }
    }
}

static void conv_is_simd(const nn_layer_t *L, const int8_t *in, int8_t *out,
                         int32_t *acc, int u0, int n, int32_t in_offset) {
    if (L->in_c == 1) {
        if (n >= 2 && L->k * L->k * L->out_c <= NN_SIMD_IS_C1TAB_MAX) {
            conv_is_c1tab(L, in, out, acc, u0, n, in_offset);   // table pays from 2 pixels
        } else {
            conv_is_c1(L, in, out, acc, u0, n, in_offset);
        }
    } else {
        conv_is_cn(L, in, out, acc, u0, n, in_offset);
    }
}

// The scalar input-stationary pool below with the pixel position kept by an
// odometer (no divides per pixel) and the channel loop on pointers. Same band,
// same opening, same additions, same rounding expression at the flush.
static void pool_is_fast(const nn_layer_t *L, const int8_t *in, int8_t *out,
                         int32_t *acc, int u0, int n) {
    const int k = L->k, c = L->in_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int32_t count = k * k;
    int iy = u0 / in_w, ix = u0 - iy * in_w;
    int oy = iy / k, ky = iy - oy * k;       // oy = iy / k, ky = iy % k
    int ox = ix / k, kx = ix - ox * k;       // ox = ix / k, kx = ix % k

    for (int p = u0; p < u0 + n; p++) {
        if (oy < out_h) {                    // else: ragged tail, if any
            if (ix == 0 && ky == 0) {        // opening this output row
                for (int i = 0; i < out_w * c; i++) acc[i] = 0;
            }
            if (ox < out_w) {
                const int8_t *ip = in + p * c;
                int32_t *ap = acc + ox * c;
                for (int ch = 0; ch < c; ch++) ap[ch] += ip[ch];
            }
            if (ix == in_w - 1 && ky == k - 1) {   // row complete
                const int32_t *a = acc;
                int8_t *o = out + oy * out_w * c;
                for (int i = 0; i < out_w * c; i++) {
                    int32_t v = *a++;
                    v = (v > 0) ? (v + count / 2) / count : (v - count / 2) / count;
                    *o++ = clamp_i8(v, -128, 127);
                }
            }
        }
        if (++kx == k) { kx = 0; ox++; }
        if (++ix == in_w) {
            ix = 0; ox = 0; kx = 0;
            if (++ky == k) { ky = 0; oy++; }
            iy++;
        }
    }
}
#endif  /* NN_SIMD */

// Packed scalar twin retained from 063.
static void k_conv_wo(const nn_layer_t *L, const int8_t *in, int8_t *out,
                      int32_t *acc, int u0, int n) {
    const int32_t in_offset = -L->in_zp;
    const int k = L->k, in_c = L->in_c, out_c = L->out_c;
    const int in_w = L->in_w, out_w = L->out_w, out_h = L->out_h;
    const int row_len = out_w * out_c;
    const uint8_t *wq = (const uint8_t *)L->w;
    const uint32_t bits = L->w_bits;

    for (int p = u0; p < u0 + n; p++) {
        const int iy = p / in_w, ix = p - iy * in_w;

        if (ix == 0 && iy < out_h) {
            int32_t *arow = acc + (iy % k) * row_len;
            for (int ox = 0; ox < out_w; ox++) {
                for (int oc = 0; oc < out_c; oc++) arow[ox * out_c + oc] = L->b[oc];
            }
        }

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
                const uint32_t wk = (uint32_t)((ky * k) + kx) * in_c;
                int32_t *ap = arow + ox * out_c;
                for (int oc = 0; oc < out_c; oc++) {
                    const uint32_t w0 = wk + (uint32_t)oc * k * k * in_c;
                    int32_t s = 0;
                    for (int c = 0; c < in_c; c++) {
                        s += nn_wo_raw(wq, bits, w0 + (uint32_t)c)
                             * ((int32_t)ip[c] + in_offset);
                    }
                    ap[oc] += s;
                }
            }
        }

        if (ix == in_w - 1) {
            const int oy = iy - k + 1;
            if (oy >= 0 && oy < out_h) {
                flush_conv_row(L, out, acc + (oy % k) * row_len, oy);
            }
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

#if NN_SIMD
    pool_is_fast(L, in, out, acc, u0, n);
    return;
#endif

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

#if NN_SIMD
// Requantize one finished dense sum and write it: the frozen kernel's two lines.
NN_SIMD_INLINE void put_fc(const nn_layer_t *L, int8_t *out, int u, int32_t sum) {
    sum = requantize_fc(sum, L->mult[u], L->shift[u]) + L->out_zp;
    out[u] = clamp_i8(sum, L->act_min, 127);
}

// G neurons starting at u: G weight rows against the one input vector, which
// is loaded and widened once per four inputs for all G.
NN_SIMD_INLINE void fc_group(const int G, const nn_layer_t *L, const int8_t *in,
                             int8_t *out, int u, uint32_t off2, int32_t off) {
    const int n_in = L->in_c;
    int32_t a[3];
    for (int j = 0; j < G; j++) a[j] = L->b[u + j];
    dot_run(G, in, L->w + (int32_t)u * n_in, n_in, n_in, off2, off, a);
    for (int j = 0; j < G; j++) put_fc(L, out, u + j, a[j]);
}

static void fc_simd(const nn_layer_t *L, const int8_t *in, int8_t *out,
                    int u0, int n, int32_t off) {
    const uint32_t off2 = nn_lanes2(off);
    const int end = u0 + n;
    int u = u0;
    while (end - u >= 3) { fc_group(3, L, in, out, u, off2, off); u += 3; }
    if (end - u == 2) fc_group(2, L, in, out, u, off2, off);
    else if (end - u == 1) fc_group(1, L, in, out, u, off2, off);
}
#endif  /* NN_SIMD */

// Packed scalar twin retained from 063.
static void k_fc_wo(const nn_layer_t *L, const int8_t *in, int8_t *out,
                    int u0, int n) {
    const int32_t in_offset = -L->in_zp;
    const int n_in = L->in_c;
    const uint8_t *wq = (const uint8_t *)L->w;
    const uint32_t bits = L->w_bits;

    for (int u = u0; u < u0 + n; u++) {
        const uint32_t w0 = (uint32_t)u * (uint32_t)n_in;
        int32_t sum = L->b[u];
        for (int i = 0; i < n_in; i++) {
            sum += nn_wo_raw(wq, bits, w0 + (uint32_t)i) * ((int32_t)in[i] + in_offset);
        }
        sum = requantize_fc(sum, L->mult[u], L->shift[u]) + L->out_zp;
        out[u] = clamp_i8(sum, L->act_min, 127);
    }
}

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

// The requantization policy THIS translation unit was compiled with -- the one
// that actually runs the requantizers. Reported from here rather than
// re-evaluated in ckpt.c, so a build that compiles ckpt.c and the kernels with
// different NN_ROUNDING still seeds with what the kernels do.
uint32_t nn_requant_policy(void) {
    return ((uint32_t)NN_RQ_CONV << 8) | (uint32_t)NN_RQ_FC;
}

// Signature adapters only: the cold selector already decided packed/SIMD.
static void k_conv_packed(const nn_layer_t *L, const int8_t *in, int8_t *out,
                          int32_t *acc, int u0, int n) {
#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    (void)acc;
    k_conv_wo(L, in, out, u0, n);
#else
    k_conv_wo(L, in, out, acc, u0, n);
#endif
}

static void k_fc_packed(const nn_layer_t *L, const int8_t *in, int8_t *out,
                        int32_t *acc, int u0, int n) {
    (void)acc;
    k_fc_wo(L, in, out, u0, n);
}

#if NN_SIMD
static void k_conv_simd(const nn_layer_t *L, const int8_t *in, int8_t *out,
                        int32_t *acc, int u0, int n) {
#if NN_DATAFLOW == NN_DATAFLOW_OUTPUT
    (void)acc;
    conv_os_simd(L, in, out, u0, n, -L->in_zp);
#else
    conv_is_simd(L, in, out, acc, u0, n, -L->in_zp);
#endif
}

static void k_fc_simd(const nn_layer_t *L, const int8_t *in, int8_t *out,
                      int32_t *acc, int u0, int n) {
    (void)acc;
    fc_simd(L, in, out, u0, n, -L->in_zp);
}
#endif

// Indexed by op id. Kept for the scalar/compile-time pool selection below.
const nn_kernel_fn nn_kernels[NN_OP_COUNT] = {
    k_conv,  // NN_OP_CONV
    k_pool,  // NN_OP_POOL
    k_fc,    // NN_OP_FC
};

nn_kernel_fn nn_select_kernel(const nn_layer_t *L, nn_path_t *path) {
    *path = path_for(L);
    switch (*path) {
    case NN_PATH_INVALID: return 0;
    case NN_PATH_PACKED_SCALAR:
        return L->op == NN_OP_CONV ? k_conv_packed : k_fc_packed;
#if NN_SIMD
    case NN_PATH_INT8_SIMD:
        return L->op == NN_OP_CONV ? k_conv_simd : k_fc_simd;
#endif
    default: return nn_kernels[L->op];
    }
}
