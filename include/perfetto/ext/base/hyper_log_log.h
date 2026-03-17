/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_HYPER_LOG_LOG_H_
#define INCLUDE_PERFETTO_EXT_BASE_HYPER_LOG_LOG_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "perfetto/base/logging.h"

// HyperLogLog is a probabilistic data structure for estimating the number of
// distinct elements (cardinality) in a multiset. It uses O(2^p) bytes of
// memory and provides estimates with a standard error of ~1.04 / sqrt(2^p).
//
// With the default precision of 11, this uses 2048 bytes and has a standard
// error of ~2.3%.
//
// Usage:
//   HyperLogLog hll;
//   for (auto hash : hashed_values) {
//     hll.Add(hash);
//   }
//   double estimate = hll.Estimate();
//
// IMPORTANT: The caller is responsible for hashing the input values before
// calling Add(). Use MurmurHashValue() from murmur_hash.h for this purpose.

namespace perfetto::base {

class HyperLogLog {
 public:
  // Precision p controls memory usage (2^p bytes) and accuracy.
  // Standard error is ~1.04 / sqrt(2^p).
  //   p=10: 1024 bytes, ~3.25% error
  //   p=11: 2048 bytes, ~2.30% error
  //   p=12: 4096 bytes, ~1.63% error
  static constexpr uint32_t kDefaultPrecision = 11;

  explicit HyperLogLog(uint32_t precision = kDefaultPrecision)
      : precision_(precision), register_count_(1u << precision) {
    PERFETTO_DCHECK(precision >= 4 && precision <= 16);
  }

  // Adds a pre-hashed value to the sketch.
  void Add(uint64_t hash) {
    if (registers_.empty()) {
      registers_.resize(register_count_, 0);
    }
    uint32_t idx = static_cast<uint32_t>(hash >> (64 - precision_));
    // Count leading zeros in the remaining bits, plus one.
    uint64_t remaining = (hash << precision_) | (1ull << (precision_ - 1));
    uint8_t rho = static_cast<uint8_t>(__builtin_clzll(remaining) + 1);
    registers_[idx] = std::max(registers_[idx], rho);
  }

  // Returns the estimated number of distinct elements added.
  double Estimate() const {
    if (registers_.empty()) {
      return 0.0;
    }

    double m = static_cast<double>(register_count_);

    // Compute the harmonic mean of 2^(-register[i]).
    double sum = 0.0;
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < register_count_; ++i) {
      sum += 1.0 / static_cast<double>(1ull << registers_[i]);
      if (registers_[i] == 0) {
        ++zeros;
      }
    }

    double alpha = AlphaMM();
    double estimate = alpha / sum;

    // Small range correction using LinearCounting.
    if (estimate <= 2.5 * m && zeros > 0) {
      estimate = m * log(m / static_cast<double>(zeros));
    }

    return estimate;
  }

  // Resets the sketch so it can be reused without reallocating.
  void Reset() { std::fill(registers_.begin(), registers_.end(), 0); }

 private:
  double AlphaMM() const {
    double m = static_cast<double>(register_count_);
    switch (register_count_) {
      case 16:
        return 0.673 * m * m;
      case 32:
        return 0.697 * m * m;
      case 64:
        return 0.709 * m * m;
      default:
        return (0.7213 / (1.0 + 1.079 / m)) * m * m;
    }
  }

  uint32_t precision_;
  uint32_t register_count_;
  std::vector<uint8_t> registers_;
};

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_HYPER_LOG_LOG_H_
