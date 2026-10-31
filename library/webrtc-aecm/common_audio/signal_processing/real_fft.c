/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "common_audio/signal_processing/include/real_fft.h"

#include <stdlib.h>

#include "common_audio/signal_processing/include/signal_processing_library.h"

// The only transform AECM needs, 128 real points, done as a 64-point radix-4
// complex FFT and a split step. A complex value is one word with the real part
// in the low half, so ARMv6 loads it whole and works on both halves at once.
// Spectra are conjugated on both sides, which is how AECM holds them.

typedef uint32_t __attribute__((may_alias)) Packed;

struct RealFFT {
  int order;
};

static const uint8_t kDigitReverse[64] = {
    0, 16, 32, 48, 4, 20, 36, 52, 8, 24, 40, 56, 12, 28, 44, 60,
    1, 17, 33, 49, 5, 21, 37, 53, 9, 25, 41, 57, 13, 29, 45, 61,
    2, 18, 34, 50, 6, 22, 38, 54, 10, 26, 42, 58, 14, 30, 46, 62,
    3, 19, 35, 51, 7, 23, 39, 55, 11, 27, 43, 59, 15, 31, 47, 63,
};

// e^(-2*pi*j*m*q/64) for m = 1, 2, 3, in Q15.
static const uint32_t kTwiddle[16][3] = {
    {0x00007fff, 0x00007fff, 0x00007fff},
    {0xf3747f61, 0xe7077d89, 0xdad87a7c},
    {0xe7077d89, 0xcf057641, 0xb8e46a6d},
    {0xdad87a7c, 0xb8e46a6d, 0x9d0f5133},
    {0xcf057641, 0xa57e5a82, 0x89bf30fb},
    {0xc3aa70e2, 0x9593471c, 0x809f0c8c},
    {0xb8e46a6d, 0x89bf30fb, 0x8277e707},
    {0xaecd62f1, 0x827718f9, 0x8f1ec3aa},
    {0xa57e5a82, 0x80010000, 0xa57ea57e},
    {0x9d0f5133, 0x8277e707, 0xc3aa8f1e},
    {0x9593471c, 0x89bfcf05, 0xe7078277},
    {0x8f1e3c56, 0x9593b8e4, 0x0c8c809f},
    {0x89bf30fb, 0xa57ea57e, 0x30fb89bf},
    {0x85842528, 0xb8e49593, 0x51339d0f},
    {0x827718f9, 0xcf0589bf, 0x6a6db8e4},
    {0x809f0c8c, 0xe7078277, 0x7a7cdad8},
};

// -j*e^(-2*pi*j*k/128) in Q15.
static const uint32_t kSplit[32] = {
    0x80010000, 0x8028f9b8, 0x809ff374, 0x8164ed38,
    0x8277e707, 0x83d7e0e6, 0x8584dad8, 0x877cd4e1,
    0x89bfcf05, 0x8c4bc946, 0x8f1ec3aa, 0x9237be32,
    0x9593b8e4, 0x9931b3c1, 0x9d0faecd, 0xa129aa0b,
    0xa57ea57e, 0xaa0ba129, 0xaecd9d0f, 0xb3c19931,
    0xb8e49593, 0xbe329237, 0xc3aa8f1e, 0xc9468c4b,
    0xcf0589bf, 0xd4e1877c, 0xdad88584, 0xe0e683d7,
    0xe7078277, 0xed388164, 0xf374809f, 0xf9b88028,
};

#if defined(WEBRTC_ARCH_ARM_V7)

#define DSP_OP(name, insn)                                     \
  static inline uint32_t name(uint32_t a, uint32_t b) {        \
    uint32_t r;                                                \
    __asm__(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));    \
    return r;                                                  \
  }

DSP_OP(Add, "sadd16")
DSP_OP(Sub, "ssub16")
DSP_OP(AddSubX, "sasx")
DSP_OP(SubAddX, "ssax")
DSP_OP(HalfAdd, "shadd16")
DSP_OP(HalfSub, "shsub16")
DSP_OP(HalfAddSubX, "shasx")
DSP_OP(HalfSubAddX, "shsax")

static inline int32_t Narrow(int32_t v) {
  int32_t r;
  __asm__("ssat %0, #16, %1, asr #15" : "=r"(r) : "r"(v));
  return r;
}

static inline uint32_t Pack(int32_t re, int32_t im) {
  uint32_t r;
  __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(Narrow(re)), "r"(Narrow(im)));
  return r;
}

// a * w
static inline uint32_t Mul(uint32_t a, uint32_t w) {
  int32_t re, im;
  __asm__("smlsd %0, %1, %2, %3" : "=r"(re) : "r"(a), "r"(w), "r"(0x4000));
  __asm__("smladx %0, %1, %2, %3" : "=r"(im) : "r"(a), "r"(w), "r"(0x4000));
  return Pack(re, im);
}

// a * conj(w)
static inline uint32_t MulConj(uint32_t a, uint32_t w) {
  int32_t re, im;
  __asm__("smlad %0, %1, %2, %3" : "=r"(re) : "r"(a), "r"(w), "r"(0x4000));
  __asm__("smlsdx %0, %1, %2, %3" : "=r"(im) : "r"(w), "r"(a), "r"(0x4000));
  return Pack(re, im);
}

static inline uint32_t Conj(uint32_t a) {
  uint32_t r;
  __asm__("ssub16 %0, %1, %2\n\tpkhbt %0, %2, %0"
          : "=&r"(r)
          : "r"(0), "r"(a));
  return r;
}

#else

static inline int32_t Re(uint32_t a) { return (int16_t)a; }
static inline int32_t Im(uint32_t a) { return (int16_t)(a >> 16); }
static inline uint32_t Join(int32_t re, int32_t im) {
  return ((uint32_t)im << 16) | ((uint32_t)re & 0xFFFF);
}

static inline uint32_t Add(uint32_t a, uint32_t b) {
  return Join(Re(a) + Re(b), Im(a) + Im(b));
}
static inline uint32_t Sub(uint32_t a, uint32_t b) {
  return Join(Re(a) - Re(b), Im(a) - Im(b));
}
static inline uint32_t AddSubX(uint32_t a, uint32_t b) {
  return Join(Re(a) - Im(b), Im(a) + Re(b));
}
static inline uint32_t SubAddX(uint32_t a, uint32_t b) {
  return Join(Re(a) + Im(b), Im(a) - Re(b));
}
static inline uint32_t HalfAdd(uint32_t a, uint32_t b) {
  return Join((Re(a) + Re(b)) >> 1, (Im(a) + Im(b)) >> 1);
}
static inline uint32_t HalfSub(uint32_t a, uint32_t b) {
  return Join((Re(a) - Re(b)) >> 1, (Im(a) - Im(b)) >> 1);
}
static inline uint32_t HalfAddSubX(uint32_t a, uint32_t b) {
  return Join((Re(a) - Im(b)) >> 1, (Im(a) + Re(b)) >> 1);
}
static inline uint32_t HalfSubAddX(uint32_t a, uint32_t b) {
  return Join((Re(a) + Im(b)) >> 1, (Im(a) - Re(b)) >> 1);
}
static inline int32_t Narrow(int32_t v) {
  v >>= 15;
  return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}
static inline uint32_t Mul(uint32_t a, uint32_t w) {
  return Join(Narrow(Re(a) * Re(w) - Im(a) * Im(w) + 0x4000),
              Narrow(Re(a) * Im(w) + Im(a) * Re(w) + 0x4000));
}
static inline uint32_t MulConj(uint32_t a, uint32_t w) {
  return Join(Narrow(Re(a) * Re(w) + Im(a) * Im(w) + 0x4000),
              Narrow(Re(w) * Im(a) - Im(w) * Re(a) + 0x4000));
}
static inline uint32_t Conj(uint32_t a) { return Join(Re(a), -Im(a)); }

#endif

static inline uint32_t SwapHalves(uint32_t a) { return (a >> 16) | (a << 16); }

#define BUTTERFLY(ADD, SUB, MINUS_J, PLUS_J, a, b, c, d) \
  do {                                                   \
    const uint32_t t0 = ADD(a, c);                       \
    const uint32_t t1 = SUB(a, c);                       \
    const uint32_t t2 = ADD(b, d);                       \
    const uint32_t t3 = SUB(b, d);                       \
    p[0] = ADD(t0, t2);                                  \
    p[l] = MINUS_J(t1, t3);                              \
    p[2 * l] = SUB(t0, t2);                              \
    p[3 * l] = PLUS_J(t1, t3);                           \
  } while (0)

// One radix-4 pass over a digit-reversed array, `l` being a quarter of the
// current sub-transform.
#define DEFINE_PASS(name, ADD, SUB, MINUS_J, PLUS_J, MUL)                    \
  static void name(Packed* x, int l) {                                       \
    const int step = 16 / l;                                                 \
    Packed* group;                                                           \
    Packed* const end = x + 64;                                              \
    for (group = x; group != end; group += 4 * l) {                          \
      Packed* p = group;                                                     \
      int q;                                                                 \
      BUTTERFLY(ADD, SUB, MINUS_J, PLUS_J, p[0], p[l], p[2 * l], p[3 * l]);  \
      for (q = 1; q < l; q++) {                                              \
        const uint32_t* w = kTwiddle[q * step];                              \
        p++;                                                                 \
        {                                                                    \
          const uint32_t b = MUL(p[l], w[0]);                                \
          const uint32_t c = MUL(p[2 * l], w[1]);                            \
          const uint32_t d = MUL(p[3 * l], w[2]);                            \
          const uint32_t a = p[0];                                           \
          BUTTERFLY(ADD, SUB, MINUS_J, PLUS_J, a, b, c, d);                  \
        }                                                                    \
      }                                                                      \
    }                                                                        \
  }

DEFINE_PASS(ForwardPass, HalfAdd, HalfSub, HalfSubAddX, HalfAddSubX, Mul)
DEFINE_PASS(InverseHalvingPass, HalfAdd, HalfSub, HalfAddSubX, HalfSubAddX, MulConj)
DEFINE_PASS(InversePass, Add, Sub, AddSubX, SubAddX, MulConj)

struct RealFFT* WebRtcSpl_CreateRealFFT(int order) {
  struct RealFFT* self = NULL;

  if (order != 7) {
    return NULL;
  }

  self = malloc(sizeof(struct RealFFT));
  if (self == NULL) {
    return NULL;
  }
  self->order = order;

  return self;
}

void WebRtcSpl_FreeRealFFT(struct RealFFT* self) {
  if (self != NULL) {
    free(self);
  }
}

int WebRtcSpl_RealForwardFFT(struct RealFFT* self,
                             const int16_t* real_data_in,
                             int16_t* complex_data_out) {
  const Packed* in = (const Packed*)real_data_in;
  Packed* x = (Packed*)complex_data_out;
  int k;
  (void)self;

  for (k = 0; k < 64; k++) {
    x[kDigitReverse[k]] = in[k];
  }
  ForwardPass(x, 1);
  ForwardPass(x, 4);
  ForwardPass(x, 16);

  for (k = 1; k < 32; k++) {
    const uint32_t a = x[k];
    const uint32_t b = SwapHalves(x[64 - k]);
    const uint32_t even = HalfSubAddX(a, b);
    const uint32_t odd = Mul(HalfAddSubX(a, b), kSplit[k]);
    x[k] = Conj(HalfAdd(even, odd));
    x[64 - k] = HalfSub(even, odd);
  }
  {
    const int32_t re = (int16_t)x[0];
    const int32_t im = (int16_t)(x[0] >> 16);
    x[0] = (uint32_t)((re + im) >> 1) & 0xFFFF;
    x[64] = (uint32_t)((re - im) >> 1) & 0xFFFF;
    x[32] = HalfAdd(x[32], 0);
  }

  return 0;
}

int WebRtcSpl_RealInverseFFT(struct RealFFT* self,
                             const int16_t* complex_data_in,
                             int16_t* real_data_out,
                             uint32_t magnitude_sum) {
  const Packed* in = (const Packed*)complex_data_in;
  Packed* x = (Packed*)real_data_out;
  // Every intermediate value is a sum of some of the bins, so `magnitude_sum`
  // bounds them all. Scale up to it, or halve in the first passes to get under.
  const uint32_t bound = magnitude_sum + (magnitude_sum >> 3);
  const int headroom = (bound ? WebRtcSpl_NormU32(bound) : 31) - 17;
  const int halved = headroom < 0 ? (1 - headroom) >> 1 : 0;
  const int up = headroom > 0 ? headroom : 0;
  const uint32_t keep = ~(((1u << up) - 1) << 16);
  int k;
  (void)self;

  for (k = 1; k < 32; k++) {
    const uint32_t a = (in[k] << up) & keep;
    const uint32_t b = SwapHalves((in[64 - k] << up) & keep);
    const uint32_t even = HalfSubAddX(a, b);
    const uint32_t odd = Mul(HalfAddSubX(a, b), kSplit[k]);
    x[kDigitReverse[k]] = Conj(HalfAdd(even, odd));
    x[kDigitReverse[64 - k]] = HalfSub(even, odd);
  }
  {
    const int32_t re = (int16_t)in[0] * (1 << up);
    const int32_t last = (int16_t)in[64] * (1 << up);
    const uint32_t middle = (in[32] << up) & keep;
    x[kDigitReverse[0]] = (((uint32_t)((re - last) >> 2)) << 16) |
                          (((uint32_t)((re + last) >> 2)) & 0xFFFF);
    x[kDigitReverse[32]] = HalfAdd(middle, 0);
  }

  if (halved > 0) {
    InverseHalvingPass(x, 1);
  } else {
    InversePass(x, 1);
  }
  if (halved > 1) {
    InverseHalvingPass(x, 4);
  } else {
    InversePass(x, 4);
  }
  InversePass(x, 16);

  return 2 + 2 * halved - up;
}
