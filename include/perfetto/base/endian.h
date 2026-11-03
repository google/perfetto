/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_BASE_ENDIAN_H_
#define INCLUDE_PERFETTO_BASE_ENDIAN_H_

#include <stdint.h>
#include <stdlib.h>  // For MSVC
#include <string.h>
#include <type_traits>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"

namespace perfetto {
namespace base {

// Reverses the byte order of a fixed-width unsigned integer.
#if PERFETTO_BUILDFLAG(PERFETTO_COMPILER_MSVC)
inline uint16_t ByteSwap16(uint16_t x) {
  return _byteswap_ushort(x);
}
inline uint32_t ByteSwap32(uint32_t x) {
  return _byteswap_ulong(x);
}
inline uint64_t ByteSwap64(uint64_t x) {
  return _byteswap_uint64(x);
}
#else
inline uint16_t ByteSwap16(uint16_t x) {
  return __builtin_bswap16(x);
}
inline uint32_t ByteSwap32(uint32_t x) {
  return __builtin_bswap32(x);
}
inline uint64_t ByteSwap64(uint64_t x) {
  return __builtin_bswap64(x);
}
#endif

// Whether |T| can be passed to the byte order helpers below.
template <typename T>
constexpr bool kIsSupportedEndianType =
    std::is_arithmetic_v<T> &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

// Reverses the byte order of an integer or floating-point value.
template <typename T>
inline T ByteSwap(T x) {
  if constexpr (sizeof(T) == 2) {
    uint16_t u;
    memcpy(&u, &x, sizeof(u));
    u = ByteSwap16(u);
    memcpy(&x, &u, sizeof(u));
  } else if constexpr (sizeof(T) == 4) {
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    u = ByteSwap32(u);
    memcpy(&x, &u, sizeof(u));
  } else if constexpr (sizeof(T) == 8) {
    uint64_t u;
    memcpy(&u, &x, sizeof(u));
    u = ByteSwap64(u);
    memcpy(&x, &u, sizeof(u));
  }
  return x;
}

// Converts |x| from host to little-endian byte order.
template <typename T>
inline T HostToLE(T x) {
  static_assert(kIsSupportedEndianType<T>, "Unsupported type");
  if constexpr (PERFETTO_IS_LITTLE_ENDIAN()) {
    return x;
  } else {
    return ByteSwap(x);
  }
}

// Converts |x| from host to big-endian byte order.
template <typename T>
inline T HostToBE(T x) {
  static_assert(kIsSupportedEndianType<T>, "Unsupported type");
  if constexpr (PERFETTO_IS_LITTLE_ENDIAN()) {
    return ByteSwap(x);
  } else {
    return x;
  }
}

// Converts |x| from little-endian to host byte order.
template <typename T>
inline T LEToHost(T x) {
  return HostToLE(x);
}

// Converts |x| from big-endian to host byte order.
template <typename T>
inline T BEToHost(T x) {
  return HostToBE(x);
}

// Reads a little-endian |T| from possibly unaligned |p|.
template <typename T>
inline T LoadLE(const void* p) {
  T v;
  memcpy(&v, p, sizeof(v));
  return LEToHost(v);
}

// Writes |v| to possibly unaligned |p| in little-endian byte order.
template <typename T>
inline void StoreLE(void* p, T v) {
  v = HostToLE(v);
  memcpy(p, &v, sizeof(v));
}

// Reads a big-endian |T| from possibly unaligned |p|.
template <typename T>
inline T LoadBE(const void* p) {
  T v;
  memcpy(&v, p, sizeof(v));
  return BEToHost(v);
}

// Writes |v| to possibly unaligned |p| in big-endian byte order.
template <typename T>
inline void StoreBE(void* p, T v) {
  v = HostToBE(v);
  memcpy(p, &v, sizeof(v));
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_ENDIAN_H_
