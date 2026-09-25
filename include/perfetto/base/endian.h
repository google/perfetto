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

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"

namespace perfetto {
namespace base {

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

#if PERFETTO_IS_LITTLE_ENDIAN()
inline uint16_t HostToLE16(uint16_t x) {
  return x;
}
inline uint32_t HostToLE32(uint32_t x) {
  return x;
}
inline uint64_t HostToLE64(uint64_t x) {
  return x;
}
inline uint16_t LE16ToHost(uint16_t x) {
  return x;
}
inline uint32_t LE32ToHost(uint32_t x) {
  return x;
}
inline uint64_t LE64ToHost(uint64_t x) {
  return x;
}
inline uint16_t HostToBE16(uint16_t x) {
  return ByteSwap16(x);
}
inline uint32_t HostToBE32(uint32_t x) {
  return ByteSwap32(x);
}
inline uint64_t HostToBE64(uint64_t x) {
  return ByteSwap64(x);
}
inline uint16_t BE16ToHost(uint16_t x) {
  return ByteSwap16(x);
}
inline uint32_t BE32ToHost(uint32_t x) {
  return ByteSwap32(x);
}
inline uint64_t BE64ToHost(uint64_t x) {
  return ByteSwap64(x);
}
inline float HostToLEFloat(float x) {
  return x;
}
inline double HostToLEDouble(double x) {
  return x;
}
inline float LEFloatToHost(float x) {
  return x;
}
inline double LEDoubleToHost(double x) {
  return x;
}
inline float HostToBEFloat(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap32(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline double HostToBEDouble(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap64(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline float BEFloatToHost(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap32(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline double BEDoubleToHost(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap64(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
#else
inline uint16_t HostToLE16(uint16_t x) {
  return ByteSwap16(x);
}
inline uint32_t HostToLE32(uint32_t x) {
  return ByteSwap32(x);
}
inline uint64_t HostToLE64(uint64_t x) {
  return ByteSwap64(x);
}
inline uint16_t LE16ToHost(uint16_t x) {
  return ByteSwap16(x);
}
inline uint32_t LE32ToHost(uint32_t x) {
  return ByteSwap32(x);
}
inline uint64_t LE64ToHost(uint64_t x) {
  return ByteSwap64(x);
}
inline uint16_t HostToBE16(uint16_t x) {
  return x;
}
inline uint32_t HostToBE32(uint32_t x) {
  return x;
}
inline uint64_t HostToBE64(uint64_t x) {
  return x;
}
inline uint16_t BE16ToHost(uint16_t x) {
  return x;
}
inline uint32_t BE32ToHost(uint32_t x) {
  return x;
}
inline uint64_t BE64ToHost(uint64_t x) {
  return x;
}
inline float HostToLEFloat(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap32(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline double HostToLEDouble(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap64(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline float LEFloatToHost(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap32(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline double LEDoubleToHost(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  u = ByteSwap64(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline float HostToBEFloat(float x) {
  return x;
}
inline double HostToBEDouble(double x) {
  return x;
}
inline float BEFloatToHost(float x) {
  return x;
}
inline double BEDoubleToHost(double x) {
  return x;
}
#endif

inline uint32_t HostToLE(uint32_t x) {
  return HostToLE32(x);
}
inline int32_t HostToLE(int32_t x) {
  return static_cast<int32_t>(HostToLE32(static_cast<uint32_t>(x)));
}
inline uint64_t HostToLE(uint64_t x) {
  return HostToLE64(x);
}
inline int64_t HostToLE(int64_t x) {
  return static_cast<int64_t>(HostToLE64(static_cast<uint64_t>(x)));
}
inline float HostToLE(float x) {
  return HostToLEFloat(x);
}
inline double HostToLE(double x) {
  return HostToLEDouble(x);
}

inline uint32_t LEToHost(uint32_t x) {
  return LE32ToHost(x);
}
inline int32_t LEToHost(int32_t x) {
  return static_cast<int32_t>(LE32ToHost(static_cast<uint32_t>(x)));
}
inline uint64_t LEToHost(uint64_t x) {
  return LE64ToHost(x);
}
inline int64_t LEToHost(int64_t x) {
  return static_cast<int64_t>(LE64ToHost(static_cast<uint64_t>(x)));
}
inline float LEToHost(float x) {
  return LEFloatToHost(x);
}
inline double LEToHost(double x) {
  return LEDoubleToHost(x);
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_ENDIAN_H_
