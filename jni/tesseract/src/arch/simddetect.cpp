///////////////////////////////////////////////////////////////////////
// File:        simddetect.cpp
// Description: Architecture detector.
// Author:      Stefan Weil (based on code from Ray Smith)
//
// (C) Copyright 2014, Google Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
///////////////////////////////////////////////////////////////////////

#include <numeric>           // for std::inner_product
#include "simddetect.h"
#include "dotproduct.h"
#include "dotproductsse.h"
#include "intsimdmatrix.h"   // for IntSimdMatrix
#include "params.h"   // for STRING_VAR
#include "tprintf.h"  // for tprintf

#if defined(AVX) || defined(AVX2) || defined(SSE4_1)
#include "dotproductavx.h"
# define HAS_CPUID
#endif

#if defined(HAS_CPUID)
#if defined(__GNUC__)
# include <cpuid.h>
#elif defined(_WIN32)
# include <intrin.h>
#endif
#endif

namespace tesseract {

// Computes and returns the dot product of the two n-vectors u and v.
// Note: because the order of addition is different among the different dot
// product functions, the results can (and do) vary slightly (although they
// agree to within about 4e-15). This produces different results when running
// training, despite all random inputs being precisely equal.
// To get consistent results, use just one of these dot product functions.
// On a test multi-layer network, serial is 57% slower than SSE, and AVX
// is about 8% faster than SSE. This suggests that the time is memory
// bandwidth constrained and could benefit from holding the reused vector
// in AVX registers.
DotProductFunction DotProduct;

static STRING_VAR(dotproduct, "auto",
                  "Function used for calculation of dot product");

SIMDDetect SIMDDetect::detector;

// If true, then AVX has been detected.
bool SIMDDetect::avx_available_;
bool SIMDDetect::avx2_available_;
bool SIMDDetect::avx512F_available_;
bool SIMDDetect::avx512BW_available_;
// If true, then SSe4.1 has been detected.
bool SIMDDetect::sse_available_;

// Computes and returns the dot product of the two n-vectors u and v.
static double DotProductGeneric(const double* u, const double* v, int n) {
  double total = 0.0;
  for (int k = 0; k < n; ++k) total += u[k] * v[k];
  return total;
}

// Compute dot product using std::inner_product.
static double DotProductStdInnerProduct(const double* u, const double* v, int n) {
  return std::inner_product(u, u + n, v, 0.0);
}

static void SetDotProduct(DotProductFunction f, const IntSimdMatrix* m = nullptr) {
  DotProduct = f;
  IntSimdMatrix::intSimdMatrix = m;
}

// Constructor.
// Tests the architecture in a system-dependent way to detect AVX, SSE and
// any other available SIMD equipment.
// __GNUC__ is also defined by compilers that include GNU extensions such as
// clang.
SIMDDetect::SIMDDetect() {
  // The fallback is a generic dot product calculation.
  SetDotProduct(DotProductGeneric);

#if defined(HAS_CPUID)
#if defined(__GNUC__)
  unsigned int eax, ebx, ecx, edx;
  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0) {
    // Note that these tests all use hex because the older compilers don't have
    // the newer flags.
#if defined(SSE4_1)
    sse_available_ = (ecx & 0x00080000) != 0;
#endif
#if defined(AVX)
    avx_available_ = (ecx & 0x10000000) != 0;
    if (avx_available_) {
      // There is supposed to be a __get_cpuid_count function, but this is all
      // there is in my cpuid.h. It is a macro for an asm statement and cannot
      // be used inside an if.
      __cpuid_count(7, 0, eax, ebx, ecx, edx);
      avx2_available_ = (ebx & 0x00000020) != 0;
      avx512F_available_ = (ebx & 0x00010000) != 0;
      avx512BW_available_ = (ebx & 0x40000000) != 0;
    }
#endif
  }
#  elif defined(_WIN32)
  int cpuInfo[4];
  int max_function_id;
  __cpuid(cpuInfo, 0);
  max_function_id = cpuInfo[0];
  if (max_function_id >= 1) {
    __cpuid(cpuInfo, 1);
#if defined(SSE4_1)
    sse_available_ = (cpuInfo[2] & 0x00080000) != 0;
#endif
#if defined(AVX)
    avx_available_ = (cpuInfo[2] & 0x10000000) != 0;
#endif
#if defined(AVX2)
    if (max_function_id >= 7) {
      __cpuid(cpuInfo, 7);
      avx2_available_ = (cpuInfo[1] & 0x00000020) != 0;
      avx512F_available_ = (cpuInfo[1] & 0x00010000) != 0;
      avx512BW_available_ = (cpuInfo[1] & 0x40000000) != 0;
    }
#endif
  }
#else
#error "I don't know how to test for SIMD with this compiler"
#endif
#endif

  // Select code for calculation of dot product based on autodetection.
  if (false) {
    // This is a dummy to support conditional compilation.
#if defined(AVX2)
  } else if (avx2_available_) {
    // AVX2 detected.
    SetDotProduct(DotProductAVX, &IntSimdMatrix::intSimdMatrixAVX2);
#endif
#if defined(AVX)
  } else if (avx_available_) {
    // AVX detected.
    SetDotProduct(DotProductAVX, &IntSimdMatrix::intSimdMatrixSSE);
#endif
#if defined(SSE4_1)
  } else if (sse_available_) {
    // SSE detected.
    SetDotProduct(DotProductSSE, &IntSimdMatrix::intSimdMatrixSSE);
#endif
  }
}

void SIMDDetect::Update() {
  // Select code for calculation of dot product based on the
  // value of the config variable if that value is not empty.
  const char* dotproduct_method = "generic";
  if (!strcmp(dotproduct.string(), "auto")) {
    // Automatic detection. Nothing to be done.
  } else if (!strcmp(dotproduct.string(), "generic")) {
    // Generic code selected by config variable.
    SetDotProduct(DotProductGeneric);
    dotproduct_method = "generic";
  } else if (!strcmp(dotproduct.string(), "native")) {
    // Native optimized code selected by config variable.
    SetDotProduct(DotProductNative);
    dotproduct_method = "native";
#if defined(AVX2)
  } else if (!strcmp(dotproduct.string(), "avx2")) {
    // AVX2 selected by config variable.
    SetDotProduct(DotProductAVX, &IntSimdMatrix::intSimdMatrixAVX2);
    dotproduct_method = "avx2";
#endif
#if defined(AVX)
  } else if (!strcmp(dotproduct.string(), "avx")) {
    // AVX selected by config variable.
    SetDotProduct(DotProductAVX, &IntSimdMatrix::intSimdMatrixSSE);
    dotproduct_method = "avx";
#endif
#if defined(SSE4_1)
  } else if (!strcmp(dotproduct.string(), "sse")) {
    // SSE selected by config variable.
    SetDotProduct(DotProductSSE, &IntSimdMatrix::intSimdMatrixSSE);
    dotproduct_method = "sse";
#endif
  } else if (!strcmp(dotproduct.string(), "std::inner_product")) {
    // std::inner_product selected by config variable.
    SetDotProduct(DotProductStdInnerProduct);
    dotproduct_method = "std::inner_product";
  } else {
    // Unsupported value of config variable.
    tprintf("Warning, ignoring unsupported config variable value: dotproduct=%s\n",
            dotproduct.string());
    tprintf("Support values for dotproduct: auto generic native"
#if defined(AVX)
            " avx"
#endif
#if defined(SSE4_1)
            " sse"
#endif
            " std::inner_product.\n");
  }

  dotproduct.set_value(dotproduct_method);
}

}  // namespace tesseract
