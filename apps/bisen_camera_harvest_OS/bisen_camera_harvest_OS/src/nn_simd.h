// nn_simd.h -- the five Cortex-M4 DSP operations the kernels use, and exact C
// versions of them for every other target. Internal to nn_kernels.c.
//
// Why these five. The M4's DSP extension multiplies two pairs of int16 and adds
// both products to an int32 in one instruction (SMLAD), and it widens two
// int8 lanes of a word to two int16 lanes in one instruction, optionally
// adding a pair of int16 on the way (SXTB16 / SXTAB16). That is the whole
// trick CMSIS-NN plays: load four int8 inputs and four int8 weights as two
// words, split each into its even and odd bytes, and do four multiply-adds in
// two instructions. The input's zero point rides along for free in SXTAB16.
//
//   word    = b0 | b1<<8 | b2<<16 | b3<<24            (little-endian load)
//   even    = sxtb16(word)            = ( b0,  b2 )   as int16 lanes
//   odd     = sxtb16(ror8(word))      = ( b1,  b3 )
//   smlad(even_w, even_x, a) + smlad(odd_w, odd_x, .) = a + w0x0 + w2x2 + w1x1 + w3x3
//
// Both operands are split the same way, so the pairs line up without the
// PKHBT/PKHTB reordering a natural-order pairing would need.
//
// Exactness. Every quantity is an integer and nothing is rounded, so the only
// ways the result could differ from the scalar loop are (a) a lane not being
// wide enough and (b) the order of the int32 additions. (a): a weight is int8
// and an input plus its offset lies in [-255, 255] for any int8 zero point, so
// both fit an int16 lane exactly; the kernels check the offset before taking
// this path (nn_simd_offset_ok). (b): integer addition is associative, so as
// long as the true sum fits int32 -- which the scalar loop also needs, since
// signed overflow is undefined there -- every order gives the same int32.
//
// Build switches:
//   NN_SIMD=0          the scalar loops of the frozen engine, byte for byte.
//   NN_SIMD=1          (default) the SIMD kernels.
//   NN_SIMD_EMULATE    default 0 on a target with the DSP extension (native
//                      instructions), 1 everywhere else (the C versions below).
//                      -DNN_SIMD_EMULATE=1 forces the C versions even on ARM.
//                      So a host build runs the SAME algorithm as the M4 build,
//                      with each instruction replaced by its exact C model.
#ifndef NN_SIMD_H
#define NN_SIMD_H

#include <stdint.h>
#include <string.h>

#ifndef NN_SIMD
#define NN_SIMD 1
#endif

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP
#define NN_SIMD_HAVE_DSP 1
#else
#define NN_SIMD_HAVE_DSP 0
#endif

#ifndef NN_SIMD_EMULATE
#define NN_SIMD_EMULATE (!NN_SIMD_HAVE_DSP)
#endif

#if NN_SIMD && !NN_SIMD_EMULATE && !NN_SIMD_HAVE_DSP
#error "NN_SIMD_EMULATE=0 needs the DSP extension (__ARM_FEATURE_DSP): build with NN_SIMD=0 or NN_SIMD_EMULATE=1"
#endif

#define NN_SIMD_INLINE static inline __attribute__((always_inline))

#if NN_SIMD && !NN_SIMD_EMULATE
// ---- native: one instruction each ------------------------------------------
// The two rotated forms are written as inline assembly because the compiler
// does not fold a separate rotate into the builtin's operand -- the ROR #8
// field of SXTB16/SXTAB16 is free, a separate ROR is one more instruction per
// word. Not volatile: they are pure functions of their inputs, so the compiler
// may schedule, share or drop them like any other arithmetic.
#include <arm_acle.h>
#define NN_SIMD_IMPL "native"

NN_SIMD_INLINE uint32_t nn_sxtb16(uint32_t x) {
    return (uint32_t)__sxtb16((int32_t)x);
}
NN_SIMD_INLINE uint32_t nn_sxtb16_ror8(uint32_t x) {
    uint32_t r;
    __asm__("sxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x));
    return r;
}
NN_SIMD_INLINE uint32_t nn_sxtab16(uint32_t a, uint32_t x) {
    return (uint32_t)__sxtab16((int32_t)a, (int32_t)x);
}
NN_SIMD_INLINE uint32_t nn_sxtab16_ror8(uint32_t a, uint32_t x) {
    uint32_t r;
    __asm__("sxtab16 %0, %1, %2, ror #8" : "=r"(r) : "r"(a), "r"(x));
    return r;
}
NN_SIMD_INLINE int32_t nn_smlad(uint32_t a, uint32_t b, int32_t acc) {
    return __smlad((int32_t)a, (int32_t)b, acc);
}
NN_SIMD_INLINE int32_t nn_smuad(uint32_t a, uint32_t b) {
    return __smuad((int32_t)a, (int32_t)b);
}

#else
// ---- emulated: the ARMv7-M Architecture Reference Manual's definitions ------
// Lane 0 is bits 15:0, lane 1 bits 31:16. All lane arithmetic is modulo 2^16
// and the SMLAD sum modulo 2^32, exactly as the instructions do it (SMLAD only
// sets the sticky Q flag on overflow; the register result wraps).
#define NN_SIMD_IMPL "emulated"

NN_SIMD_INLINE uint32_t nn_ror8(uint32_t x) { return (x >> 8) | (x << 24); }

// SignExtend(byte) as an int16 bit pattern.
NN_SIMD_INLINE uint32_t nn_sx8(uint32_t b) {
    return (uint32_t)(uint16_t)(int16_t)(int8_t)(uint8_t)b;
}

// SXTB16: lane0 = SignExtend(x[7:0]), lane1 = SignExtend(x[23:16]).
NN_SIMD_INLINE uint32_t nn_sxtb16(uint32_t x) {
    return nn_sx8(x & 0xFFu) | (nn_sx8((x >> 16) & 0xFFu) << 16);
}
NN_SIMD_INLINE uint32_t nn_sxtb16_ror8(uint32_t x) { return nn_sxtb16(nn_ror8(x)); }

// SXTAB16: lane i = a.lane i + SignExtend(byte 2i of x), each modulo 2^16.
NN_SIMD_INLINE uint32_t nn_sxtab16(uint32_t a, uint32_t x) {
    const uint32_t lo = ((a & 0xFFFFu) + nn_sx8(x & 0xFFu)) & 0xFFFFu;
    const uint32_t hi = ((a >> 16) + nn_sx8((x >> 16) & 0xFFu)) & 0xFFFFu;
    return lo | (hi << 16);
}
NN_SIMD_INLINE uint32_t nn_sxtab16_ror8(uint32_t a, uint32_t x) {
    return nn_sxtab16(a, nn_ror8(x));
}

// SMLAD: acc + a.lane0*b.lane0 + a.lane1*b.lane1, lanes signed, result mod 2^32.
NN_SIMD_INLINE int32_t nn_smlad(uint32_t a, uint32_t b, int32_t acc) {
    const int32_t p0 = (int32_t)(int16_t)(uint16_t)(a & 0xFFFFu) *
                       (int32_t)(int16_t)(uint16_t)(b & 0xFFFFu);
    const int32_t p1 = (int32_t)(int16_t)(uint16_t)(a >> 16) *
                       (int32_t)(int16_t)(uint16_t)(b >> 16);
    return (int32_t)((uint32_t)acc + (uint32_t)p0 + (uint32_t)p1);
}
// SMUAD: SMLAD with no accumulator.
NN_SIMD_INLINE int32_t nn_smuad(uint32_t a, uint32_t b) { return nn_smlad(a, b, 0); }
#endif

// int32 addition modulo 2^32, which is what the M4's ADD and SMLAD do. The
// SIMD kernels sum in a different ORDER from the scalar loop, so a partial sum
// of theirs could leave the int32 range in a case where every partial sum of
// the scalar loop stays inside it; as plain `+=` that would be undefined
// behaviour in C even though the final sum fits. Every accumulation on the SIMD
// path goes through this instead, so it is defined for every input and equals
// the scalar result whenever the scalar loop itself is defined. Compiles to the
// same single ADD. (The unsigned-to-signed conversion is implementation-defined,
// not undefined; GCC defines it as modulo 2^32.)
NN_SIMD_INLINE int32_t nn_add(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

// Two int8 at any alignment as one int16 pair (lane 0 = p[0], lane 1 = p[1]):
// a halfword load, then byte 1 copied up to byte 2 so SXTB16 picks bytes 0 and
// 2. Three instructions on the M4.
NN_SIMD_INLINE uint32_t nn_rd_pair(const int8_t *p) {
    uint16_t h;
    memcpy(&h, p, sizeof h);
    const uint32_t v = (uint32_t)h;
    return nn_sxtb16(v | (v << 8));
}

// Two int16-range values as one pair (lane 0 = lo).
NN_SIMD_INLINE uint32_t nn_pack2(int32_t lo, int32_t hi) {
    return ((uint32_t)lo & 0xFFFFu) | ((uint32_t)hi << 16);
}

// Four int8 at any alignment, as one little-endian word. memcpy compiles to a
// single LDR on the M4, which allows unaligned word loads (CCR.UNALIGN_TRP is
// clear at reset and nothing in this firmware sets it); the weights of
// channel j start at j*k*k*in_c, so most are not word aligned.
NN_SIMD_INLINE uint32_t nn_rd32(const int8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

// A value the compiler must treat as unknown at this point. Emits nothing. Used
// in the window copy, which GCC otherwise turns into memcpy() library calls
// (one per 5- or 30-byte row -- slower than the inline word copy and outside
// the code this file controls). Native builds only; the emulated build has no
// reason to care what the copy compiles to.
#if NN_SIMD && !NN_SIMD_EMULATE
#define NN_SIMD_OPAQUE(v) __asm__("" : "+r"(v))
#else
#define NN_SIMD_OPAQUE(v) ((void)0)
#endif

// The input offset in both int16 lanes, for SXTAB16.
NN_SIMD_INLINE uint32_t nn_lanes2(int32_t v) {
    return ((uint32_t)v & 0xFFFFu) | (((uint32_t)v & 0xFFFFu) << 16);
}

// The dual-lane path is exact only if every input plus the offset fits an
// int16 lane: x + off in [-128 + off, 127 + off] within [-32768, 32767]. Any
// int8 zero point passes; a descriptor with a wilder one takes the scalar loop.
NN_SIMD_INLINE int nn_simd_offset_ok(int32_t off) {
    return off >= -32640 && off <= 32640;
}

#endif  // NN_SIMD_H
