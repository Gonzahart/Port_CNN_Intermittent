// nn_wo.h -- weight-only low-bit storage: the format and its decoder (engine-063-wo-v1).
//
// A conv or dense layer may store its weights at N bits each instead of 8, N in
// 2..7. The integers are the ones the int8 container model already holds -- per
// output channel, symmetric, in [-L, L] with L = 2^(N-1) - 1 -- so the arithmetic
// does not change: each weight is decoded to an int32 and multiplied exactly as
// the int8 path multiplies. Only the storage shrinks. Activations stay int8 and
// accumulation stays int32.
//
// FORMAT, version 1. Everything a decoder needs is stated here.
//
//   Width     nn_layer_t.w_bits.
//               0     legacy descriptor: an int8 array. Headers written by the
//                     frozen exporter end their initialisers before this field,
//                     so it is 0 there, and so is w_len.
//               8     an int8 array, declared (int8 layers of a packed model).
//               2..7  packed, as below.
//             Any other value is refused at admission.
//   Order     element i is the i-th weight of the int8 array the frozen exporter
//             emits for that layer -- [oc][ky][kx][ic] for conv, [out][in] for
//             dense. Packing never reorders, so every kernel index is unchanged.
//   Coding    N-bit two's complement, valid values -L..L. The code 2^(N-1), which
//             would decode to -(L+1), is UNUSED: symmetric quantization never
//             produces it. For N = 2 (ternary) that is 0b10. A layer holding it is
//             refused.
//   Bit order element i occupies bits [i*N, i*N + N) of a little-endian bit
//             stream: stream bit b is bit (b % 8) of byte (b / 8), and the
//             element's least significant bit is its lowest stream bit.
//   Length    nn_layer_t.w_len = ceil(count * N / 8) bytes. The exporter emits it
//             as sizeof() of the array itself, so the two cannot disagree in a
//             generated header.
//   Padding   the unused high bits of the last byte are zero; anything else is
//             refused.
//
// The decoder is STATELESS: weight i is a pure function of (bytes, N, i). A resume
// at any unit needs no decoder state and a checkpoint never holds any. It reads
// byte (i*N)/8 and, only when the element crosses into it, the next byte -- never
// past the element's own last byte, so a packed array is never overread.
//
// nn_wo_raw is the unchecked form the kernels use, after admission (nn_begin via
// nn_weights_admissible) has proved every descriptor and every code valid.
// nn_wo_get is the checked form: admission, the checkpoint layer and the tests.
#ifndef NN_WO_H
#define NN_WO_H

#include <stdint.h>

#define NN_WO_VERSION  1u
#define NN_WO_MIN_BITS 2u
#define NN_WO_MAX_BITS 7u

// Is this descriptor a packed one? (0 and 8 are int8 arrays.)
#define NN_WO_LAYER_PACKED(L) \
    ((L)->w_bits >= NN_WO_MIN_BITS && (L)->w_bits <= NN_WO_MAX_BITS)

// Bytes that `count` elements of `bits` each occupy.
static inline uint32_t nn_wo_len(uint32_t count, uint32_t bits) {
    return (uint32_t)(((uint64_t)count * bits + 7u) / 8u);
}

// Unchecked decode of element i. Requires bits in 2..8 and an i whose element
// lies inside the array. Sign extension is (v ^ s) - s on the N-bit field.
static inline int32_t nn_wo_raw(const uint8_t *p, uint32_t bits, uint32_t i) {
    const uint32_t bit = i * bits;
    const uint32_t at = bit >> 3, sh = bit & 7u;
    uint32_t v = (uint32_t)p[at] >> sh;
    if (sh + bits > 8u) v |= (uint32_t)p[at + 1u] << (8u - sh);
    const uint32_t s = 1u << (bits - 1u);
    v &= (s << 1) - 1u;
    return (int32_t)(v ^ s) - (int32_t)s;
}

// Checked decode. 0 and *out on success; -1 for a null array, a width outside
// 2..8, a length that is not exactly ceil(count*bits/8), an index outside the
// array, or the unused code.
static inline int nn_wo_get(const uint8_t *p, uint32_t len, uint32_t bits,
                            uint32_t count, uint32_t i, int32_t *out) {
    if (!p || bits < NN_WO_MIN_BITS || bits > 8u) return -1;
    if (len != nn_wo_len(count, bits) || i >= count) return -1;
    const int32_t v = nn_wo_raw(p, bits, i);
    if (v == -(int32_t)(1u << (bits - 1u))) return -1;   // the unused code
    *out = v;
    return 0;
}

// 1 if every bit after the `count`-th element in the last byte is zero.
static inline int nn_wo_padding_zero(const uint8_t *p, uint32_t len,
                                     uint32_t bits, uint32_t count) {
    if (len == 0u) return count == 0u;
    const uint32_t tail = (uint32_t)(((uint64_t)count * bits) & 7u);
    if (tail == 0u) return 1;                // the last element ends a byte
    return ((uint32_t)p[len - 1u] >> tail) == 0u;
}

#endif  // NN_WO_H
