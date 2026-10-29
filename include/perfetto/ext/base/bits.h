/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_EXT_BASE_BITS_H_
#define INCLUDE_PERFETTO_EXT_BASE_BITS_H_

#include <cstddef>
#include <cstdint>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"

namespace perfetto {
namespace base {

inline PERFETTO_ALWAYS_INLINE uint32_t CountLeadZeros32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return value ? static_cast<uint32_t>(__builtin_clz(value)) : 32u;
#else
  unsigned long out;
  return _BitScanReverse(&out, value) ? 31 - out : 32u;
#endif
}

inline PERFETTO_ALWAYS_INLINE uint32_t CountLeadZeros64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return value ? static_cast<uint32_t>(__builtin_clzll(value)) : 64u;
#else
  unsigned long out;
  return _BitScanReverse64(&out, value) ? 63 - out : 64u;
#endif
}

template <typename T>
uint32_t CountLeadZeros(T value) {
  if constexpr (sizeof(T) == 8)
    return CountLeadZeros64(static_cast<uint64_t>(value));
  return CountLeadZeros32(static_cast<uint32_t>(value));
}

inline PERFETTO_ALWAYS_INLINE uint32_t CountTrailZeros64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return value ? static_cast<uint32_t>(__builtin_ctzll(value)) : 64u;
#else
  unsigned long out;
  return _BitScanForward64(&out, value) ? static_cast<uint32_t>(out) : 64u;
#endif
}

inline PERFETTO_ALWAYS_INLINE uint32_t CountTrailZeros32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return value ? static_cast<uint32_t>(__builtin_ctz(value)) : 32u;
#else
  unsigned long out;
  return _BitScanForward(&out, value) ? out : 32u;
#endif
}

template <typename T>
inline PERFETTO_ALWAYS_INLINE uint32_t CountTrailZeros(T value) {
  if (sizeof(T) == 8) {
    return CountTrailZeros64(static_cast<uint64_t>(value));
  }
  return CountTrailZeros32(static_cast<uint32_t>(value));
}

// Software emulation of the x64 PEXT instruction. Extracts bits from |word|
// at positions where |mask| has set bits, packing them into the low bits.
// See https://www.felixcloutier.com/x86/pext for details.
//
// Scales with the number of set bits in |mask| rather than being constant
// time: prefer Pext64(), which uses the real instruction where available.
inline PERFETTO_ALWAYS_INLINE uint64_t PextSlow64(uint64_t word,
                                                  uint64_t mask) {
  if (word == 0 || mask == ~uint64_t(0)) {
    return word;
  }
  uint64_t result = 0;
  for (uint64_t bb = 1; mask; bb += bb) {
    if (word & mask & (0ull - mask)) {
      result |= bb;
    }
    mask &= mask - 1;
  }
  return result;
}

inline PERFETTO_ALWAYS_INLINE uint64_t Pext64(uint64_t word, uint64_t mask) {
#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT) && \
    (defined(__GNUC__) || defined(__clang__))
  return __builtin_ia32_pext_di(word, mask);
#else
  return PextSlow64(word, mask);
#endif
}

template <typename T>
constexpr bool AllBitsSet(T v) {
  return v == static_cast<T>(-1);
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_BITS_H_
