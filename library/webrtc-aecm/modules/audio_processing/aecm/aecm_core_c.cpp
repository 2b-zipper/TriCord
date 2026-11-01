/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "common_audio/signal_processing/include/signal_processing_library.h"
#include "common_audio/signal_processing/include/spl_inl.h"
#include "modules/audio_processing/aecm/aecm_core.h"
#include "modules/audio_processing/aecm/aecm_defines.h"
#include "modules/audio_processing/aecm/echo_control_mobile.h"
#include "modules/audio_processing/utility/delay_estimator_wrapper.h"
#include "rtc_base/checks.h"
#include "rtc_base/numerics/safe_conversions.h"
#include "rtc_base/sanitizer.h"

extern "C" {
#include "common_audio/signal_processing/include/real_fft.h"
}

namespace webrtc {

namespace {

typedef uint32_t __attribute__((may_alias)) SamplePair;

// Square root of Hanning window in Q14.
constexpr int16_t kSqrtHanning[PART_LEN1] = {
    0,     399,   798,   1196,  1594,  1990,  2386,  2780,  3172,  3562,  3951,
    4337,  4720,  5101,  5478,  5853,  6224,  6591,  6954,  7313,  7668,  8019,
    8364,  8705,  9040,  9370,  9695,  10013, 10326, 10633, 10933, 11227, 11514,
    11795, 12068, 12335, 12594, 12845, 13089, 13325, 13553, 13773, 13985, 14189,
    14384, 14571, 14749, 14918, 15079, 15231, 15373, 15506, 15631, 15746, 15851,
    15947, 16034, 16111, 16179, 16237, 16286, 16325, 16354, 16373, 16384};

struct AnalysisWindow {
  alignas(4) int16_t coefficients[PART_LEN2];
  constexpr AnalysisWindow() : coefficients() {
    for (int i = 0; i < PART_LEN; i++) {
      coefficients[i] = kSqrtHanning[i];
      coefficients[PART_LEN + i] = kSqrtHanning[PART_LEN - i];
    }
  }
};
constexpr AnalysisWindow kWindow;

// 2^23 / x, for x the top byte of a normalized divisor plus a half.
struct ByteInverse {
  uint16_t values[128];
  constexpr ByteInverse() : values() {
    for (int i = 0; i < 128; i++) {
      values[i] = (uint16_t)((1 << 24) / (2 * (128 + i) + 1));
    }
  }
};
constexpr ByteInverse kByteInverse;

// `WebRtcSpl_NormW16` of the largest sample, two samples at a time.
int NormOfBlock(const int16_t* time_signal) {
  const SamplePair* in = (const SamplePair*)time_signal;
  uint32_t bits = 0;

  for (int i = 0; i < PART_LEN; i++) {
    bits |= in[i] ^ (in[i] << 1);
  }
  bits = (bits | (bits >> 16)) & 0xFFFF;
  return bits ? WebRtcSpl_NormU32(bits) - 16 : 0;
}

void WindowAndFFT(AecmCore* aecm,
                  int16_t* fft,
                  const int16_t* time_signal,
                  ComplexInt16* freq_signal,
                  int time_signal_scaling) {
  const SamplePair* in = (const SamplePair*)time_signal;
  const SamplePair* window = (const SamplePair*)kWindow.coefficients;
  SamplePair* out = (SamplePair*)fft;
  const int shift = time_signal_scaling + 2;

  for (int i = 0; i < PART_LEN; i++) {
    const uint32_t samples = in[i];
    const uint32_t gains = window[i];
    const int32_t low = (int16_t)samples * (int16_t)gains;
    const int32_t high = ((int32_t)samples >> 16) * ((int32_t)gains >> 16);
    out[i] = ((uint32_t)(high * (1 << shift)) & 0xFFFF0000) |
             ((uint32_t)(low * (1 << shift)) >> 16);
  }

  WebRtcSpl_RealForwardFFT(aecm->real_fft, fft, (int16_t*)freq_signal);
}

void InverseFFTAndWindow(AecmCore* aecm,
                         int16_t* ifft_out,
                         const ComplexInt16* efw,
                         int16_t* output,
                         const int16_t* nearendClean,
                         uint32_t magnitudeSum) {
  const int outCFFT = WebRtcSpl_RealInverseFFT(
      aecm->real_fft, (const int16_t*)efw, ifft_out, magnitudeSum);
  const int shift = WEBRTC_SPL_MIN(14 + aecm->dfaCleanQDomain - outCFFT, 31);
  const int32_t rounding = 1 << (shift - 1);

  for (int i = 0; i < PART_LEN; i++) {
    int32_t sample =
        (ifft_out[i] * kWindow.coefficients[i] + rounding) >> shift;
    sample += aecm->outBuf[i];
    output[i] = (int16_t)WEBRTC_SPL_SAT(WEBRTC_SPL_WORD16_MAX, sample,
                                        WEBRTC_SPL_WORD16_MIN);

    sample = (ifft_out[PART_LEN + i] * kWindow.coefficients[PART_LEN + i] +
              rounding) >>
             shift;
    aecm->outBuf[i] = (int16_t)WEBRTC_SPL_SAT(WEBRTC_SPL_WORD16_MAX, sample,
                                              WEBRTC_SPL_WORD16_MIN);
  }

  // Copy the current block to the old position
  // (aecm->outBuf is shifted elsewhere)
  memcpy(aecm->xBuf, aecm->xBuf + PART_LEN, sizeof(int16_t) * PART_LEN);
  memcpy(aecm->dBufNoisy, aecm->dBufNoisy + PART_LEN,
         sizeof(int16_t) * PART_LEN);
  if (nearendClean != nullptr) {
    memcpy(aecm->dBufClean, aecm->dBufClean + PART_LEN,
           sizeof(int16_t) * PART_LEN);
  }
}

// Transforms a time domain signal into the frequency domain, outputting the
// complex valued signal, absolute value and sum of absolute values.
//
// time_signal          [in]    Pointer to time domain signal
// freq_signal_real     [out]   Pointer to real part of frequency domain array
// freq_signal_imag     [out]   Pointer to imaginary part of frequency domain
//                              array
// freq_signal_abs      [out]   Pointer to absolute value of frequency domain
//                              array
// freq_signal_sum_abs  [out]   Pointer to the sum of all absolute values in
//                              the frequency domain array
// return value                 The Q-domain of current frequency values
//
int TimeToFrequencyDomain(AecmCore* aecm,
                          const int16_t* time_signal,
                          ComplexInt16* freq_signal,
                          uint16_t* freq_signal_abs,
                          uint32_t* freq_signal_sum_abs) {
  int16_t fft_buf[PART_LEN4 + 16];  // +16 for 32-byte alignment.
  int16_t* fft = (int16_t*)(((uintptr_t)fft_buf + 31) & ~31);
  int time_signal_scaling = 0;

#ifdef AECM_DYNAMIC_Q
  time_signal_scaling = NormOfBlock(time_signal);
#endif

  WindowAndFFT(aecm, fft, time_signal, freq_signal, time_signal_scaling);

  freq_signal_abs[0] = (uint16_t)WEBRTC_SPL_ABS_W16(freq_signal[0].real);
  freq_signal_abs[PART_LEN] =
      (uint16_t)WEBRTC_SPL_ABS_W16(freq_signal[PART_LEN].real);
  (*freq_signal_sum_abs) =
      (uint32_t)(freq_signal_abs[0]) + (uint32_t)(freq_signal_abs[PART_LEN]);

  // Within 7% of the true magnitude, and well within what AECM can resolve.
  for (int i = 1; i < PART_LEN; i++) {
    const int32_t real = WEBRTC_SPL_ABS_W16(freq_signal[i].real);
    const int32_t imag = WEBRTC_SPL_ABS_W16(freq_signal[i].imag);
    const int32_t high = real > imag ? real : imag;
    const int32_t low = real > imag ? imag : real;
    freq_signal_abs[i] = (uint16_t)(high + (low >> 2) + (low >> 3));
    (*freq_signal_sum_abs) += (uint32_t)freq_signal_abs[i];
  }

  return time_signal_scaling;
}

// `num` / `den` to about eight bits, for both non-zero.
inline uint32_t ApproxDivide(uint32_t num, uint16_t den) {
  const int den_zeros = WebRtcSpl_NormU32(den);
  const int num_zeros = WebRtcSpl_NormU32(num);
  const int shift = num_zeros - den_zeros + 31;
  const uint32_t inverse =
      kByteInverse.values[(((uint32_t)den << den_zeros) >> 24) - 128];
  return shift > 31 ? 0 : (((num << num_zeros) >> 16) * inverse) >> shift;
}

bool IsSilent(const int16_t* block) {
  int bits = 0;
  for (int i = 0; i < PART_LEN; i++) {
    bits |= block[i];
  }
  return bits == 0;
}

// The next block adds its first half to the overlap of one that never went
// through the transform.
void LeaveIdle(AecmCore* aecm, const uint16_t* dfaNoisy) {
  for (int i = 0; i < PART_LEN; i++) {
    const int32_t window = kSqrtHanning[PART_LEN - i];
    aecm->outBuf[i] = (int16_t)((((aecm->dBufNoisy[i] * window) >> 14) * window) >> 14);
  }
  aecm->idle = 0;
}

}  // namespace

int RTC_NO_SANITIZE("signed-integer-overflow")  // bugs.webrtc.org/8200
    WebRtcAecm_ProcessBlock(AecmCore* aecm,
                            const int16_t* farend,
                            const int16_t* nearendNoisy,
                            const int16_t* nearendClean,
                            int16_t* output) {
  int i;

  uint32_t xfaSum;
  uint32_t dfaNoisySum;
  uint32_t dfaCleanSum;
  uint32_t echoEst32Gained;
  uint32_t tmpU32;

  int32_t tmp32no1;

  uint16_t xfa[PART_LEN1];
  uint16_t dfaNoisy[PART_LEN1];
  uint16_t dfaClean[PART_LEN1];
  uint16_t* ptrDfaClean = dfaClean;
  const uint16_t* far_spectrum_ptr = nullptr;

  // 32 byte aligned buffers (with +8 or +16).
  // TODO(kma): define fft with ComplexInt16.
  int16_t fft_buf[PART_LEN4 + 2 + 16];  // +2 to make a loop safe.
  int32_t echoEst32_buf[PART_LEN1 + 8];
  int32_t dfw_buf[PART_LEN2 + 8];
  int32_t efw_buf[PART_LEN2 + 8];

  int16_t* fft = (int16_t*)(((uintptr_t)fft_buf + 31) & ~31);
  int32_t* echoEst32 = (int32_t*)(((uintptr_t)echoEst32_buf + 31) & ~31);
  ComplexInt16* dfw = (ComplexInt16*)(((uintptr_t)dfw_buf + 31) & ~31);
  ComplexInt16* efw = (ComplexInt16*)(((uintptr_t)efw_buf + 31) & ~31);

  int16_t hnl[PART_LEN1];
  int16_t numPosCoef = 0;
  int delay;
  int16_t tmp16no1;
  int16_t tmp16no2;
  int16_t mu;
  int16_t supGain;
  int16_t zeros32, zeros16;
  int16_t zerosDBufNoisy, zerosDBufClean, zerosXBuf;
  int far_q;
  int16_t resolutionDiff, qDomainDiff, dfa_clean_q_domain_diff;

  const int kMinPrefBand = 4;
  const int kMaxPrefBand = 24;
  int32_t avgHnl32 = 0;

  // Once the far end has left the history there is nothing to cancel, and unit
  // gain through the transform would only return the previous near block.
  aecm->farSilentBlocks = IsSilent(farend) ? aecm->farSilentBlocks + 1 : 0;
  if (aecm->farSilentBlocks >= MAX_DELAY + 2) {
    if (!aecm->idle) {
      aecm->idle = 1;
      aecm->supGain = 0;
      aecm->supGainOld = 0;
      memset(aecm->echoFilt, 0, sizeof(aecm->echoFilt));
    }
    memcpy(output, aecm->dBufNoisy, sizeof(int16_t) * PART_LEN);
    memcpy(aecm->dBufNoisy, nearendNoisy, sizeof(int16_t) * PART_LEN);
    return 0;
  }

  // Determine startup state. There are three states:
  // (0) the first CONV_LEN blocks
  // (1) another CONV_LEN blocks
  // (2) the rest

  if (aecm->startupState < 2) {
    aecm->startupState =
        (aecm->totCount >= CONV_LEN) + (aecm->totCount >= CONV_LEN2);
  }
  // END: Determine startup state

  // Buffer near and far end signals
  memcpy(aecm->xBuf + PART_LEN, farend, sizeof(int16_t) * PART_LEN);
  memcpy(aecm->dBufNoisy + PART_LEN, nearendNoisy, sizeof(int16_t) * PART_LEN);
  if (nearendClean != nullptr) {
    memcpy(aecm->dBufClean + PART_LEN, nearendClean,
           sizeof(int16_t) * PART_LEN);
  }

  // Transform far end signal from time domain to frequency domain.
  far_q = TimeToFrequencyDomain(aecm, aecm->xBuf, dfw, xfa, &xfaSum);

  // Transform noisy near end signal from time domain to frequency domain.
  zerosDBufNoisy =
      TimeToFrequencyDomain(aecm, aecm->dBufNoisy, dfw, dfaNoisy, &dfaNoisySum);
  aecm->dfaNoisyQDomainOld = aecm->dfaNoisyQDomain;
  aecm->dfaNoisyQDomain = (int16_t)zerosDBufNoisy;
  if (aecm->idle) {
    LeaveIdle(aecm, dfaNoisy);
  }

  if (nearendClean == nullptr) {
    ptrDfaClean = dfaNoisy;
    aecm->dfaCleanQDomainOld = aecm->dfaNoisyQDomainOld;
    aecm->dfaCleanQDomain = aecm->dfaNoisyQDomain;
    dfaCleanSum = dfaNoisySum;
  } else {
    // Transform clean near end signal from time domain to frequency domain.
    zerosDBufClean = TimeToFrequencyDomain(aecm, aecm->dBufClean, dfw, dfaClean,
                                           &dfaCleanSum);
    aecm->dfaCleanQDomainOld = aecm->dfaCleanQDomain;
    aecm->dfaCleanQDomain = (int16_t)zerosDBufClean;
  }

  // Get the delay
  // Save far-end history and estimate delay
  WebRtcAecm_UpdateFarHistory(aecm, xfa, far_q);
  if (WebRtc_AddFarSpectrumFix(aecm->delay_estimator_farend, xfa, PART_LEN1,
                               far_q) == -1) {
    return -1;
  }
  aecm->delayTurn = !aecm->delayTurn;
  delay = aecm->delayTurn
              ? WebRtc_last_delay(aecm->delay_estimator)
              : WebRtc_DelayEstimatorProcessFix(aecm->delay_estimator, dfaNoisy,
                                                PART_LEN1, zerosDBufNoisy);
  if (delay == -1) {
    return -1;
  } else if (delay == -2) {
    // If the delay is unknown, we assume zero.
    // NOTE: this will have to be adjusted if we ever add lookahead.
    delay = 0;
  }

  if (aecm->fixedDelay >= 0) {
    // Use fixed delay
    delay = aecm->fixedDelay;
  }

  // Get aligned far end spectrum
  far_spectrum_ptr = WebRtcAecm_AlignedFarend(aecm, &far_q, delay);
  zerosXBuf = (int16_t)far_q;
  if (far_spectrum_ptr == nullptr) {
    return -1;
  }

  // Calculate log(energy) and update energy threshold levels
  WebRtcAecm_CalcEnergies(aecm, far_spectrum_ptr, zerosXBuf, dfaNoisySum,
                          echoEst32);

  // Calculate stepsize
  mu = WebRtcAecm_CalcStepSize(aecm);

  // Update counters
  aecm->totCount += aecm->currentVADValue;

  // This is the channel estimation algorithm.
  // It is base on NLMS but has a variable step length,
  // which was calculated above.
  WebRtcAecm_UpdateChannel(aecm, far_spectrum_ptr, zerosXBuf, dfaNoisy, mu,
                           echoEst32);
  supGain = WebRtcAecm_CalcSuppressionGain(aecm);

  // Calculate Wiener filter hnl[]
  const int16_t supGainZeros = WebRtcSpl_NormW16(supGain) + 1;
  dfa_clean_q_domain_diff = aecm->dfaCleanQDomain - aecm->dfaCleanQDomainOld;
  for (i = 0; i < PART_LEN1; i++) {
    // Far end signal through channel estimate in Q8
    // How much can we shift right to preserve resolution
    tmp32no1 = echoEst32[i] - aecm->echoFilt[i];
    aecm->echoFilt[i] += dchecked_cast<int32_t>((int64_t{tmp32no1} * 50) >> 8);

    zeros32 = WebRtcSpl_NormW32(aecm->echoFilt[i]) + 1;
    zeros16 = supGainZeros;
    if (zeros32 + zeros16 > 16) {
      // Multiplication is safe
      // Result in
      // Q(RESOLUTION_CHANNEL+RESOLUTION_SUPGAIN+
      //   aecm->xfaQDomainBuf[diff])
      echoEst32Gained =
          WEBRTC_SPL_UMUL_32_16((uint32_t)aecm->echoFilt[i], (uint16_t)supGain);
      resolutionDiff = 14 - RESOLUTION_CHANNEL16 - RESOLUTION_SUPGAIN;
      resolutionDiff += (aecm->dfaCleanQDomain - zerosXBuf);
    } else {
      tmp16no1 = 17 - zeros32 - zeros16;
      resolutionDiff =
          14 + tmp16no1 - RESOLUTION_CHANNEL16 - RESOLUTION_SUPGAIN;
      resolutionDiff += (aecm->dfaCleanQDomain - zerosXBuf);
      if (zeros32 > tmp16no1) {
        echoEst32Gained = WEBRTC_SPL_UMUL_32_16((uint32_t)aecm->echoFilt[i],
                                                supGain >> tmp16no1);
      } else {
        // Result in Q-(RESOLUTION_CHANNEL+RESOLUTION_SUPGAIN-16)
        echoEst32Gained = (aecm->echoFilt[i] >> tmp16no1) * supGain;
      }
    }

    zeros16 = WebRtcSpl_NormW16(aecm->nearFilt[i]);
    RTC_DCHECK_GE(zeros16, 0);  // `zeros16` is a norm, hence non-negative.
    if (zeros16 < dfa_clean_q_domain_diff && aecm->nearFilt[i]) {
      tmp16no1 = aecm->nearFilt[i] * (1 << zeros16);
      qDomainDiff = zeros16 - dfa_clean_q_domain_diff;
      tmp16no2 = ptrDfaClean[i] >> -qDomainDiff;
    } else {
      tmp16no1 = dfa_clean_q_domain_diff < 0
                     ? aecm->nearFilt[i] >> -dfa_clean_q_domain_diff
                     : aecm->nearFilt[i] * (1 << dfa_clean_q_domain_diff);
      qDomainDiff = 0;
      tmp16no2 = ptrDfaClean[i];
    }
    tmp32no1 = (int32_t)(tmp16no2 - tmp16no1);
    tmp16no2 = (int16_t)(tmp32no1 >> 4);
    tmp16no2 += tmp16no1;
    zeros16 = WebRtcSpl_NormW16(tmp16no2);
    if ((tmp16no2) & (-qDomainDiff > zeros16)) {
      aecm->nearFilt[i] = WEBRTC_SPL_WORD16_MAX;
    } else {
      aecm->nearFilt[i] = qDomainDiff < 0 ? tmp16no2 * (1 << -qDomainDiff)
                                          : tmp16no2 >> qDomainDiff;
    }

    // Wiener filter coefficients, resulting hnl in Q14
    if (echoEst32Gained == 0) {
      hnl[i] = ONE_Q14;
    } else if (aecm->nearFilt[i] == 0) {
      hnl[i] = 0;
    } else {
      // Multiply the suppression gain
      // Rounding
      echoEst32Gained += (uint32_t)(aecm->nearFilt[i] >> 1);
      tmpU32 = ApproxDivide(echoEst32Gained, (uint16_t)aecm->nearFilt[i]);

      // Current resolution is
      // Q-(RESOLUTION_CHANNEL+RESOLUTION_SUPGAIN- max(0,17-zeros16- zeros32))
      // Make sure we are in Q14
      tmp32no1 = (int32_t)WEBRTC_SPL_SHIFT_W32(tmpU32, resolutionDiff);
      if (tmp32no1 > ONE_Q14) {
        hnl[i] = 0;
      } else if (tmp32no1 < 0) {
        hnl[i] = ONE_Q14;
      } else {
        // 1-echoEst/dfa
        hnl[i] = ONE_Q14 - (int16_t)tmp32no1;
        if (hnl[i] < 0) {
          hnl[i] = 0;
        }
      }
    }
    if (hnl[i]) {
      numPosCoef++;
    }
  }
  // Only in wideband. Prevent the gain in upper band from being larger than
  // in lower band.
  if (aecm->mult == 2) {
    // TODO(bjornv): Investigate if the scaling of hnl[i] below can cause
    //               speech distortion in double-talk.
    for (i = 0; i < PART_LEN1; i++) {
      hnl[i] = (int16_t)((hnl[i] * hnl[i]) >> 14);
    }

    for (i = kMinPrefBand; i <= kMaxPrefBand; i++) {
      avgHnl32 += (int32_t)hnl[i];
    }
    RTC_DCHECK_GT(kMaxPrefBand - kMinPrefBand + 1, 0);
    avgHnl32 /= (kMaxPrefBand - kMinPrefBand + 1);

    for (i = kMaxPrefBand; i < PART_LEN1; i++) {
      if (hnl[i] > (int16_t)avgHnl32) {
        hnl[i] = (int16_t)avgHnl32;
      }
    }
  }

  // Calculate NLP gain, result is in Q14
  if (aecm->nlpFlag) {
    for (i = 0; i < PART_LEN1; i++) {
      // Truncate values close to zero and one, and remove outliers.
      if (hnl[i] > NLP_COMP_HIGH) {
        hnl[i] = ONE_Q14;
      } else if (hnl[i] < NLP_COMP_LOW || numPosCoef < 3) {
        hnl[i] = 0;
      }

      // multiply with Wiener coefficients
      if (hnl[i] == ONE_Q14) {
        efw[i] = dfw[i];
      } else if (hnl[i] == 0) {
        efw[i].real = 0;
        efw[i].imag = 0;
      } else {
        efw[i].real = (int16_t)(WEBRTC_SPL_MUL_16_16_RSFT_WITH_ROUND(
            dfw[i].real, hnl[i], 14));
        efw[i].imag = (int16_t)(WEBRTC_SPL_MUL_16_16_RSFT_WITH_ROUND(
            dfw[i].imag, hnl[i], 14));
      }
    }
  } else {
    // multiply with Wiener coefficients
    for (i = 0; i < PART_LEN1; i++) {
      efw[i].real = (int16_t)(WEBRTC_SPL_MUL_16_16_RSFT_WITH_ROUND(dfw[i].real,
                                                                   hnl[i], 14));
      efw[i].imag = (int16_t)(WEBRTC_SPL_MUL_16_16_RSFT_WITH_ROUND(dfw[i].imag,
                                                                   hnl[i], 14));
    }
  }

  InverseFFTAndWindow(aecm, fft, efw, output, nearendClean, dfaCleanSum);

  return 0;
}

}  // namespace webrtc
