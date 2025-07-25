/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_UTIL_HYPER_LOG_LOG_H_
#define SRC_TRACE_PROCESSOR_UTIL_HYPER_LOG_LOG_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/util/murmur_hash.h"

namespace perfetto {
namespace trace_processor {
namespace util {

// HyperLogLog (HLL) is a probabilistic algorithm for estimating the number of
// distinct elements (cardinality) in a multiset.
//
// This implementation is based on the paper "HyperLogLog: the analysis of a
// near-optimal cardinality estimation algorithm" by Flajolet et al.
//
// The algorithm works by hashing each element and using the hash to update a
// set of registers. The first `p` bits of the hash determine which register to
// update, and the number of leading zeros in the remaining bits determines the
// value to store in the register (specifically, the number of leading zeros +
// 1). The maximum value seen for each register is stored.
//
// The cardinality is then estimated using the harmonic mean of the register
// values. This estimate is corrected for small cardinalities.
class HyperLogLog {
 public:
  // The precision `p` defines the number of registers `m = 2^p`.
  // The value of `p` must be between 4 and 16, inclusive. A larger `p`
  // value leads to higher accuracy at the cost of more memory.
  // The relative error is approximately 1.04 / sqrt(m).
  //
  // The default of 13 provides a good trade-off between accuracy and memory.
  explicit HyperLogLog(uint8_t p = 12)
      : p_(p), m_(1 << p), alpha_(GetAlpha(m_)) {
    PERFETTO_CHECK(p_ >= 4 && p_ <= 16);
  }

  HyperLogLog(HyperLogLog&&) = default;
  HyperLogLog& operator=(HyperLogLog&&) = default;

  HyperLogLog(const HyperLogLog&) = delete;
  HyperLogLog& operator=(const HyperLogLog&) = delete;

  // Adds a value to the sketch. The value is hashed before being added.
  template <typename T>
  void Add(const T& value) {
    AddPrehashed(util::MurmurHash(value));
  }

  // Adds a pre-hashed value to the sketch.
  void AddPrehashed(uint64_t hash) {
    if (PERFETTO_UNLIKELY(registers_.empty())) {
      registers_.resize(m_, 0);
    }

    // Use the first `p` bits of the hash to select a register.
    uint32_t index = static_cast<uint32_t>(hash >> (64 - p_));

    // The rank is the number of leading zeros in the remaining bits of the
    // hash, plus one.
    uint64_t w_shifted = hash << p_;
    uint8_t rank;
    if (w_shifted == 0) {
      // Handle the case where the last (64 - p_) bits are all zero.
      // The rank is the number of bits in w + 1.
      rank = (64 - p_) + 1;
    } else {
      // The rank is the number of leading zeros in the shifted hash + 1.
      rank = static_cast<uint8_t>(Lzcnt64(w_shifted)) + 1;
    }

    // Store the maximum rank seen for this register.
    registers_[index] = std::max(registers_[index], rank);
  }

  // Estimates the cardinality of the set.
  double Estimate() const {
    if (registers_.empty()) {
      return 0.0;
    }

    double sum = 0.0;
    uint32_t zeros = 0;
    for (uint8_t rank : registers_) {
      zeros += rank == 0;
      sum += 1.0 / static_cast<double>(1ull << rank);
    }

    double estimate = alpha_ * m_ * m_ / sum;

    // ONLY apply small-range correction if registers are still empty
    // AND the estimate is within the HLL's known biased range.
    if (zeros != 0 && estimate <= 2.5 * m_) {
      return m_ * std::log(static_cast<double>(m_) / zeros);
    }

    // Otherwise, for medium and large cardinalities, always trust the raw
    // estimate.
    return estimate;
  }

  // Resets the sketch to its initial state, allowing for reuse of the
  // allocated memory.
  void Reset() { std::fill(registers_.begin(), registers_.end(), 0); }

 private:
  // Counts the number of leading zeros in a 64-bit integer.
  PERFETTO_ALWAYS_INLINE uint32_t Lzcnt64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return value ? static_cast<uint32_t>(__builtin_clzll(value)) : 64u;
#else
    unsigned long out;
    return _BitScanReverse64(&out, value) ? 63 - out : 64u;
#endif
  }

  // Gets the alpha constant for the given number of registers.
  constexpr double GetAlpha(uint32_t m) {
    switch (m) {
      case 16:
        return 0.673;
      case 32:
        return 0.697;
      case 64:
        return 0.709;
      default:
        return 0.7213 / (1.0 + 1.079 / m);
    }
  }

  // Precision value.
  uint8_t p_;

  // Number of registers (m = 2^p).
  uint32_t m_;

  // Alpha constant for bias correction.
  double alpha_;

  // HLL registers.
  std::vector<uint8_t> registers_;
};

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_UTIL_HYPER_LOG_LOG_H_
