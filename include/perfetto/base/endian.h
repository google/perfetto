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

static_assert(sizeof(short) == 2, "Unsupported short size");
static_assert(sizeof(int) == 4, "Unsupported int size");
static_assert(sizeof(long) == sizeof(int) || sizeof(long) == sizeof(long long),
              "Unsupported long size");
static_assert(sizeof(long long) == 8, "Unsupported long long size");
static_assert(sizeof(float) == 4, "Unsupported float size");
static_assert(sizeof(double) == 8, "Unsupported double size");

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
inline unsigned short HostToLE(unsigned short x) {
  return x;
}
inline unsigned int HostToLE(unsigned int x) {
  return x;
}
inline unsigned long long HostToLE(unsigned long long x) {
  return x;
}
inline unsigned short HostToBE(unsigned short x) {
  return ByteSwap16(x);
}
inline unsigned int HostToBE(unsigned int x) {
  return ByteSwap32(x);
}
inline unsigned long long HostToBE(unsigned long long x) {
  return ByteSwap64(x);
}
#else
inline unsigned short HostToLE(unsigned short x) {
  return ByteSwap16(x);
}
inline unsigned int HostToLE(unsigned int x) {
  return ByteSwap32(x);
}
inline unsigned long long HostToLE(unsigned long long x) {
  return ByteSwap64(x);
}
inline unsigned short HostToBE(unsigned short x) {
  return x;
}
inline unsigned int HostToBE(unsigned int x) {
  return x;
}
inline unsigned long long HostToBE(unsigned long long x) {
  return x;
}
#endif

inline bool HostToLE(bool x) {
  return x;
}
inline char HostToLE(char x) {
  return x;
}
inline signed char HostToLE(signed char x) {
  return x;
}
inline unsigned char HostToLE(unsigned char x) {
  return x;
}
inline short HostToLE(short x) {
  return static_cast<short>(HostToLE(static_cast<unsigned short>(x)));
}
inline int HostToLE(int x) {
  return static_cast<int>(HostToLE(static_cast<unsigned int>(x)));
}
inline long long HostToLE(long long x) {
  return static_cast<long long>(HostToLE(static_cast<unsigned long long>(x)));
}
inline unsigned long HostToLE(unsigned long x) {
  if (sizeof(long) == sizeof(int))
    return HostToLE(static_cast<unsigned int>(x));
  return static_cast<unsigned long>(
      HostToLE(static_cast<unsigned long long>(x)));
}
inline long HostToLE(long x) {
  return static_cast<long>(HostToLE(static_cast<unsigned long>(x)));
}
inline float HostToLE(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  u = HostToLE(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline double HostToLE(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  u = HostToLE(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}

inline bool HostToBE(bool x) {
  return x;
}
inline char HostToBE(char x) {
  return x;
}
inline signed char HostToBE(signed char x) {
  return x;
}
inline unsigned char HostToBE(unsigned char x) {
  return x;
}
inline short HostToBE(short x) {
  return static_cast<short>(HostToBE(static_cast<unsigned short>(x)));
}
inline int HostToBE(int x) {
  return static_cast<int>(HostToBE(static_cast<unsigned int>(x)));
}
inline long long HostToBE(long long x) {
  return static_cast<long long>(HostToBE(static_cast<unsigned long long>(x)));
}
inline unsigned long HostToBE(unsigned long x) {
  if (sizeof(long) == sizeof(int))
    return HostToBE(static_cast<unsigned int>(x));
  return static_cast<unsigned long>(
      HostToBE(static_cast<unsigned long long>(x)));
}
inline long HostToBE(long x) {
  return static_cast<long>(HostToBE(static_cast<unsigned long>(x)));
}
inline float HostToBE(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  u = HostToBE(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}
inline double HostToBE(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  u = HostToBE(u);
  memcpy(&x, &u, sizeof(u));
  return x;
}

template <typename T>
inline T LEToHost(T x) {
  static_assert(std::is_same_v<T, decltype(HostToLE(x))>,
                "T must have a matching endian overload");
  return HostToLE(x);
}

template <typename T>
inline T BEToHost(T x) {
  static_assert(std::is_same_v<T, decltype(HostToBE(x))>,
                "T must have a matching endian overload");
  return HostToBE(x);
}

template <typename T>
inline T LoadLE(const void* p) {
  T v;
  memcpy(&v, p, sizeof(v));
  return LEToHost(v);
}

template <typename T>
inline void StoreLE(void* p, T v) {
  static_assert(std::is_same_v<T, decltype(HostToLE(v))>,
                "T must have a matching endian overload");
  v = HostToLE(v);
  memcpy(p, &v, sizeof(v));
}

template <typename T>
inline T LoadBE(const void* p) {
  T v;
  memcpy(&v, p, sizeof(v));
  return BEToHost(v);
}

template <typename T>
inline void StoreBE(void* p, T v) {
  static_assert(std::is_same_v<T, decltype(HostToBE(v))>,
                "T must have a matching endian overload");
  v = HostToBE(v);
  memcpy(p, &v, sizeof(v));
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_ENDIAN_H_
