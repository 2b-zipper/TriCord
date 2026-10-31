/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef COMMON_AUDIO_SIGNAL_PROCESSING_INCLUDE_REAL_FFT_H_
#define COMMON_AUDIO_SIGNAL_PROCESSING_INCLUDE_REAL_FFT_H_

#include <stdint.h>

struct RealFFT;

#ifdef __cplusplus
extern "C" {
#endif

// Only order 7, 128 real samples, is supported.
struct RealFFT* WebRtcSpl_CreateRealFFT(int order);
void WebRtcSpl_FreeRealFFT(struct RealFFT* self);

// Writes bins 0 to 64 of conj(DFT(x)) / 128 to `complex_data_out`, real then
// imaginary. `real_data_in` must be 4-byte aligned and is not modified; the
// two buffers must not overlap.
//
// Return Value:
//   0
int WebRtcSpl_RealForwardFFT(struct RealFFT* self,
                             const int16_t* real_data_in,
                             int16_t* complex_data_out);

// Undoes a forward transform, without the division by 128. `complex_data_in`
// must be 4-byte aligned and must not overlap `real_data_out`. `magnitude_sum`
// is the sum of the magnitudes of the input bins, or an estimate no more than
// 10% low. It is what sets the scaling, in place of searching for the peak.
//
// Return Value:
//   How far left the elements of `real_data_out` should be shifted to get
//   correct physical values. Can be negative.
int WebRtcSpl_RealInverseFFT(struct RealFFT* self,
                             const int16_t* complex_data_in,
                             int16_t* real_data_out,
                             uint32_t magnitude_sum);

#ifdef __cplusplus
}
#endif

#endif  // COMMON_AUDIO_SIGNAL_PROCESSING_INCLUDE_REAL_FFT_H_
