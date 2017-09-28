/*
 * This host implementation is an extract from the version in the 
 * Eigen library @ https://bitbucket.org/eigen/
 * File: /eigen/Eigen/src/Core/arch/CUDA/Half.h
 * Version: 699b6595fc471456896fb27193c8ca51389b7850
 * Removed all non-used code
 */ 


// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// The conversion routines are Copyright (c) Fabian Giesen, 2016.
// The original license follows:
//
// Copyright (c) Fabian Giesen, 2016
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


// Standard 16-bit float type, mostly useful for GPUs. Defines a new
// type Eigen::half (inheriting from CUDA's __half struct) with
// operator overloads such that it behaves basically as an arithmetic
// type. It will be quite slow on CPUs (so it is recommended to stay
// in fp32 for CPUs, except for simple parameter conversions, I/O
// to disk and the likes), but fast on GPUs.


#ifndef EIGEN_HALF_HOST
#define EIGEN_HALF_host

#define EIGEN_STRONG_INLINE static inline
#define EIGEN_DEVICE_FUNC



#if __cplusplus > 199711L
#define EIGEN_EXPLICIT_CAST(tgt_type) explicit operator tgt_type()
#else
#define EIGEN_EXPLICIT_CAST(tgt_type) operator tgt_type()
#endif


namespace Eigen {

struct half;

namespace half_impl {

#if !defined(EIGEN_HAS_CUDA_FP16)
// Make our own __half_raw definition that is similar to CUDA's.
struct __half_raw {
  EIGEN_DEVICE_FUNC __half_raw() : x(0) {}
  explicit EIGEN_DEVICE_FUNC __half_raw(unsigned short raw) : x(raw) {}
  unsigned short x;
};
#elif defined(EIGEN_CUDACC_VER) && EIGEN_CUDACC_VER < 90000
// In CUDA < 9.0, __half is the equivalent of CUDA 9's __half_raw
typedef __half __half_raw;
#endif

EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC __half_raw raw_uint16_to_half(unsigned short x);
EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC __half_raw float_to_half_rtne(float ff);
EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC float half_to_float(__half_raw h);

struct half_base : public __half_raw {
  EIGEN_DEVICE_FUNC half_base() {}
  EIGEN_DEVICE_FUNC half_base(const half_base& h) : __half_raw(h) {}
  EIGEN_DEVICE_FUNC half_base(const __half_raw& h) : __half_raw(h) {}
#if defined(EIGEN_HAS_CUDA_FP16) && defined(EIGEN_CUDACC_VER) && EIGEN_CUDACC_VER >= 90000
  EIGEN_DEVICE_FUNC half_base(const __half& h) : __half_raw(*(__half_raw*)&h) {}
#endif
};

} // namespace half_impl


namespace half_impl {


// Conversion routines, including fallbacks for the host or older CUDA.
// Note that newer Intel CPUs (Haswell or newer) have vectorized versions of
// these in hardware. If we need more performance on older/other CPUs, they are
// also possible to vectorize directly.

EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC __half_raw raw_uint16_to_half(unsigned short x) {
  __half_raw h;
  h.x = x;
  return h;
}

union FP32 {
  unsigned int u;
  float f;
};

EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC __half_raw float_to_half_rtne(float ff) {
#if defined(EIGEN_HAS_CUDA_FP16) && defined(EIGEN_CUDA_ARCH) && EIGEN_CUDA_ARCH >= 300
  __half tmp_ff = __float2half(ff);
  return *(__half_raw*)&tmp_ff;

#elif defined(EIGEN_HAS_FP16_C)
  __half_raw h;
  h.x = _cvtss_sh(ff, 0);
  return h;

#else
  FP32 f; f.f = ff;

  const FP32 f32infty = { 255 << 23 };
  const FP32 f16max = { (127 + 16) << 23 };
  const FP32 denorm_magic = { ((127 - 15) + (23 - 10) + 1) << 23 };
  unsigned int sign_mask = 0x80000000u;
  __half_raw o;
  o.x = static_cast<unsigned short>(0x0u);

  unsigned int sign = f.u & sign_mask;
  f.u ^= sign;

  // NOTE all the integer compares in this function can be safely
  // compiled into signed compares since all operands are below
  // 0x80000000. Important if you want fast straight SSE2 code
  // (since there's no unsigned PCMPGTD).

  if (f.u >= f16max.u) {  // result is Inf or NaN (all exponent bits set)
    o.x = (f.u > f32infty.u) ? 0x7e00 : 0x7c00; // NaN->qNaN and Inf->Inf
  } else {  // (De)normalized number or zero
    if (f.u < (113 << 23)) {  // resulting FP16 is subnormal or zero
      // use a magic value to align our 10 mantissa bits at the bottom of
      // the float. as long as FP addition is round-to-nearest-even this
      // just works.
      f.f += denorm_magic.f;

      // and one integer subtract of the bias later, we have our final float!
      o.x = static_cast<unsigned short>(f.u - denorm_magic.u);
    } else {
      unsigned int mant_odd = (f.u >> 13) & 1; // resulting mantissa is odd

      // update exponent, rounding bias part 1
      f.u += ((unsigned int)(15 - 127) << 23) + 0xfff;
      // rounding bias part 2
      f.u += mant_odd;
      // take the bits!
      o.x = static_cast<unsigned short>(f.u >> 13);
    }
  }

  o.x |= static_cast<unsigned short>(sign >> 16);
  return o;
#endif
}

EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC float half_to_float(__half_raw h) {
#if defined(EIGEN_HAS_CUDA_FP16) && defined(EIGEN_CUDA_ARCH) && EIGEN_CUDA_ARCH >= 300
  return __half2float(h);

#elif defined(EIGEN_HAS_FP16_C)
  return _cvtsh_ss(h.x);

#else
  const FP32 magic = { 113 << 23 };
  const unsigned int shifted_exp = 0x7c00 << 13; // exponent mask after shift
  FP32 o;

  o.u = (h.x & 0x7fff) << 13;             // exponent/mantissa bits
  unsigned int exp = shifted_exp & o.u;   // just the exponent
  o.u += (127 - 15) << 23;                // exponent adjust

  // handle exponent special cases
  if (exp == shifted_exp) {     // Inf/NaN?
    o.u += (128 - 16) << 23;    // extra exp adjust
  } else if (exp == 0) {        // Zero/Denormal?
    o.u += 1 << 23;             // extra exp adjust
    o.f -= magic.f;             // renormalize
  }

  o.u |= (h.x & 0x8000) << 16;    // sign bit
  return o.f;
#endif
}
} //End half_impl
} // end Eigen

#endif // EIGEN_HALF_HOST
