/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_COMPILE_TIME_HASH_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_COMPILE_TIME_HASH_H_

#include <stddef.h>
#include <stdint.h>

namespace perfetto {
namespace internal {

// A helper class which computes a 64-bit hash of the input data at compile
// time. The algorithm used is FNV-1a as it is fast and easy to implement and
// has relatively few collisions.
// WARNING: This hash function should not be used for any cryptographic purpose.
class CompileTimeHash {
 public:
  // Creates an empty hash object
  constexpr inline CompileTimeHash() {}

  // Hashes a byte array.
  constexpr inline CompileTimeHash Update(const char* data, size_t size) const {
    return CompileTimeHash(HashRecursively(kFnv1a64OffsetBasis, data, size));
  }

  constexpr inline uint64_t digest() const { return result_; }

 private:
  constexpr inline CompileTimeHash(uint64_t result) : result_(result) {}

  static constexpr inline uint64_t HashRecursively(uint64_t value,
                                                   const char* data,
                                                   size_t size) {
    return !size ? value
                 : HashRecursively(
                       (value ^ static_cast<uint8_t>(*data)) * kFnv1a64Prime,
                       data + 1, size - 1);
  }

  static constexpr uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325;
  static constexpr uint64_t kFnv1a64Prime = 0x100000001b3;

  uint64_t result_ = kFnv1a64OffsetBasis;
};

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_COMPILE_TIME_HASH_H_
