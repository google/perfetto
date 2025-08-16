/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_UTIL_MURMURHASH_H_
#define SRC_TRACE_PROCESSOR_UTIL_MURMURHASH_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

// This file implements a 64-bit variant of the MurmurHash algorithm.
//
// The algorithm is a custom hybrid that combines elements from both MurmurHash2
// and MurmurHash3 to achieve excellent performance for non-cryptographic use
// cases. It is heavily inspired by the high-performance hash implementation
// found in DuckDB.
//
// --- Algorithm Comparison ---
//
// This implementation differs from the standard MurmurHash algorithms:
//
// - vs. MurmurHash2: It uses the same primary multiplication constant
//   (0xc6a4a7935bd1e995) as MurmurHash2 but features a simpler body loop
//   (a single XOR and multiply) and the stronger `fmix64` finalizer from
//   MurmurHash3.
//
// - vs. MurmurHash3: It uses the exact same `fmix64` finalization function but
//   substitutes MurmurHash3's complex, rotation-heavy body loop with a much
//   simpler and faster one.
//
// In summary, it makes a performance-oriented trade-off: a simpler main loop
// combined with a high-quality final mixing stage.
//
// ⚠️ NOTE: This implementation is NOT cryptographically secure. It must not be
// used for security-sensitive applications like password storage or digital
// signatures, as it is not designed to be resistant to malicious attacks.

namespace perfetto::trace_processor::util {

// Finalizes an intermediate hash value using the `fmix64` routine from
// MurmurHash3.
//
// This function's purpose is to thoroughly mix the bits of the hash state to
// ensure the final result is well-distributed, which is critical for avoiding
// collisions in hash tables.
//
// Args:
//   h: The intermediate hash value to be finalized.
//
// Returns:
//   The final, well-mixed 64-bit hash value.
inline uint64_t MurmurHash(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

// Computes a 64-bit hash for a block of memory.
//
// This function implements the main body of the custom Murmur-style hash. As
// described in the file-level comment, it uses a simplified processing loop for
// performance and applies the strong `fmix64` finalizer from MurmurHash3.
//
// The process involves four steps:
// 1. Initialization: Seeding the hash with the input length.
// 2. Main Loop: Processing 8-byte chunks with a `XOR` and `MULTIPLY` sequence.
// 3. Tail Processing: Handling the final 1-7 bytes.
// 4. Finalization: Applying the `fmix64` mix via the other MurmurHash overload.
//
// Args:
//   input: A pointer to the data to be hashed.
//   len:   The length of the data in bytes.
//
// Returns:
//   The 64-bit hash of the input data.
inline uint64_t MurmurHash(const void* input, size_t len) {
  // Uses constants inspired by the high-performance hash implementation found
  // at:
  // https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/common/types/hash.hpp
  constexpr uint64_t kMultiplicationConstant1 = 0xc6a4a7935bd1e995ULL;
  constexpr uint64_t kMultiplicationConstant2 = 0xd6e8feb86659fd93ULL;
  constexpr uint64_t kSeed = 0xe17a1465U;

  // Initialize the hash value with the seed and a transformation of the input
  // length. This helps ensure that inputs of different lengths are unlikely to
  // collide.
  uint64_t hash_value = kSeed ^ (len * kMultiplicationConstant1);

  // Set up pointers for iterating through the data.
  const auto* byte_ptr = static_cast<const uint8_t*>(input);
  const size_t remainder = len % 8;
  const uint8_t* end = byte_ptr + len - remainder;

  // The main loop processes data in 8-byte blocks for performance. Each block
  // is XORed and multiplied into the hash state.
  for (; byte_ptr != end; byte_ptr += 8) {
    uint64_t chunk;
    memcpy(&chunk, byte_ptr, sizeof(chunk));
    hash_value ^= chunk;
    hash_value *= kMultiplicationConstant2;
  }

  // Handle the final 1-7 bytes if the data length is not a multiple of 8.
  // This ensures that all input bytes contribute to the final hash.
  if (remainder != 0) {
    uint64_t last_chunk = 0;
    memcpy(&last_chunk, byte_ptr, remainder);
    hash_value ^= last_chunk;
    hash_value *= kMultiplicationConstant2;
  }

  // Finalize the hash by calling the integer-based MurmurHash function to
  // perform the final mixing.
  return MurmurHash(hash_value);
}

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_MURMURHASH_H_
