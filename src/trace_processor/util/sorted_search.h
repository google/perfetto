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

#ifndef SRC_TRACE_PROCESSOR_UTIL_SORTED_SEARCH_H_
#define SRC_TRACE_PROCESSOR_UTIL_SORTED_SEARCH_H_

#include <cstddef>
#include <cstdint>

#include "perfetto/base/compiler.h"
#include "perfetto/public/compiler.h"

namespace perfetto::trace_processor {

// The first of [first, last) for which `less(element, value)` is false, as
// std::lower_bound finds it, calling `less` the same way.
//
// With `kCheap` comparisons, the answer is always in [base, base + n]. Each
// step looks at one element and keeps the half the answer is in, but n
// halves whatever the comparison says, so the comparison only decides where
// base moves: one load, one comparison and one conditional move a step, with
// the number of steps fixed by the length alone. That costs one comparison
// more than the fewest possible. Without PERFETTO_UNPREDICTABLE, clang turns
// the move into a branch, which mispredicts about every other step on keys
// which vary.
//
// Otherwise a comparison costs more than a mispredicted branch, so each step
// shrinks the range by what the comparison says, for the fewest comparisons.
template <bool kCheap, typename It, typename V, typename Less>
PERFETTO_ALWAYS_INLINE It
LowerBound(It first, It last, const V& value, Less less) {
  auto n = static_cast<size_t>(last - first);
  if constexpr (kCheap) {
    if (n == 0) {
      return first;
    }
    while (n > 1) {
      size_t half = n / 2;
      first = PERFETTO_UNPREDICTABLE(less(first[half], value)) ? first + half
                                                               : first;
      n -= half;
    }
    return first + less(*first, value);
  } else {
    while (n > 0) {
      size_t half = n / 2;
      if (less(first[half], value)) {
        first += half + 1;
        n -= half + 1;
      } else {
        n = half;
      }
    }
    return first;
  }
}

// The first of [first, last) for which `less(value, element)` is true, as
// std::upper_bound finds it, calling `less` the same way. See LowerBound.
template <bool kCheap, typename It, typename V, typename Less>
PERFETTO_ALWAYS_INLINE It
UpperBound(It first, It last, const V& value, Less less) {
  auto n = static_cast<size_t>(last - first);
  if constexpr (kCheap) {
    if (n == 0) {
      return first;
    }
    while (n > 1) {
      size_t half = n / 2;
      first = PERFETTO_UNPREDICTABLE(!less(value, first[half])) ? first + half
                                                                : first;
      n -= half;
    }
    return first + !less(value, *first);
  } else {
    while (n > 0) {
      size_t half = n / 2;
      if (!less(value, first[half])) {
        first += half + 1;
        n -= half + 1;
      } else {
        n = half;
      }
    }
    return first;
  }
}

// Ranges this short are scanned rather than halved: the elements share a cache
// line or two, so comparing each is cheaper than branching on halves.
inline constexpr size_t kGallopingLinearScan = 16;

// std::partition_point over [begin, end), where `pred` holds for a prefix of
// the range: binary search until few elements are left, then a scan.
template <typename T, typename Pred>
const T* PartitionPoint(const T* begin, const T* end, Pred pred) {
  while (static_cast<size_t>(end - begin) > kGallopingLinearScan) {
    const T* mid = begin + (end - begin) / 2;
    if (pred(*mid)) {
      begin = mid + 1;
    } else {
      end = mid;
    }
  }
  while (begin != end && pred(*begin)) {
    ++begin;
  }
  return begin;
}

namespace internal {

// The rest of GallopingPartitionPoint, once `pred` is known to hold for the
// first kGallopingLinearScan elements: probes at doubling distances, then
// searches the last gap. Out of line, so callers inline only the scan.
template <typename T, typename Pred>
PERFETTO_NO_INLINE const T* Gallop(const T* begin, const T* end, Pred pred) {
  auto size = static_cast<size_t>(end - begin);
  size_t lo = kGallopingLinearScan;
  size_t hi = size;
  for (size_t step = kGallopingLinearScan; lo + step < size; step *= 2) {
    if (!pred(begin[lo + step])) {
      hi = lo + step;
      break;
    }
    lo += step + 1;
  }
  return PartitionPoint(begin + lo, begin + hi, pred);
}

}  // namespace internal

// As PartitionPoint, but in O(log d) for a point d elements from `begin`:
// scans the first few elements, then probes at doubling distances and
// searches only the last gap. For when the point is likely near `begin`.
template <typename T, typename Pred>
PERFETTO_ALWAYS_INLINE inline const T* GallopingPartitionPoint(const T* begin,
                                                               const T* end,
                                                               Pred pred) {
  // A fixed trip count, not min(size, kGallopingLinearScan), so the scan
  // unrolls with nothing to compute before it.
  for (size_t i = 0; i < kGallopingLinearScan; ++i) {
    if (begin + i == end || !pred(begin[i])) {
      return begin + i;
    }
  }
  return internal::Gallop(begin, end, pred);
}

// Galloping (exponential) search optimized for sorted queries.
//
// When searching for multiple keys that are themselves sorted, galloping search
// exploits locality: each search starts from where the previous one ended,
// using exponential probing to quickly find the new range, then binary search
// within that range.
//
// Performance: O(log d) per query where d is the distance between consecutive
// keys. Much faster than repeated std::lower_bound for sorted query batches.
class GallopingSearch {
 public:
  GallopingSearch(const int64_t* data, uint32_t size) : data_(data), n_(size) {}

  // Keys MUST be sorted in ascending order for this to work correctly.
  // Results[i] will contain the lower_bound position for keys[i].
  void BatchedLowerBound(const int64_t* keys,
                         uint32_t num_keys,
                         uint32_t* results) const {
    if (n_ == 0) {
      for (uint32_t i = 0; i < num_keys; ++i) {
        results[i] = 0;
      }
      return;
    }
    uint32_t pos = LowerBound(0, n_, keys[0]);
    results[0] = pos;
    for (uint32_t i = 1; i < num_keys; ++i) {
      pos = GallopForward(pos, keys[i]);
      results[i] = pos;
    }
  }

 private:
  static constexpr uint32_t kLinearScanThreshold = 16;

  uint32_t GallopForward(uint32_t pos, int64_t key) const {
    if (pos >= n_ || data_[pos] >= key) {
      return pos;
    }
    // Start at cache-line granularity (16 elements = 2 cache lines of int64_t)
    // since nearby elements are already paged in when we access data[pos].
    uint32_t step = kLinearScanThreshold;
    uint32_t prev = pos;
    while (pos + step < n_ && data_[pos + step] < key) {
      prev = pos + step;
      step *= 2;
    }
    uint32_t lo = prev + 1;
    uint32_t hi = (pos + step + 1 > n_) ? n_ : pos + step + 1;
    return LowerBound(lo, hi, key);
  }

  uint32_t LowerBound(uint32_t lo, uint32_t hi, int64_t key) const {
    // Binary search until range is small enough for linear scan.
    while (hi - lo > kLinearScanThreshold) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (data_[mid] < key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    // Linear scan for small ranges.
    while (lo < hi && data_[lo] < key) {
      ++lo;
    }
    return lo;
  }

  const int64_t* data_ = nullptr;
  uint32_t n_ = 0;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_SORTED_SEARCH_H_
