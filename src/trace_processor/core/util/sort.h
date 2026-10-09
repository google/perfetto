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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_SORT_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_SORT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "perfetto/ext/base/bits.h"

namespace perfetto::trace_processor::core {
namespace internal {

// Splits a key of `key_bits` bits into digits of at most 16 bits, all the
// same width. Each pass scatters every element, which costs several times
// clearing and summing one count, so the split minimizes
// passes * (4 * size + counts): the fewest passes for many elements, and
// narrower digits, with more passes, for few.
struct RadixDigits {
  uint32_t passes;
  uint32_t bits;
};
inline RadixDigits GetRadixDigits(uint32_t key_bits, size_t size) {
  uint32_t fewest = (key_bits + 15) / 16;
  if (fewest == 0) {
    return {0, 0};
  }
  RadixDigits best{};
  uint64_t best_cost = std::numeric_limits<uint64_t>::max();
  for (uint32_t passes = fewest; passes <= fewest + 2; ++passes) {
    uint32_t bits = (key_bits + passes - 1) / passes;
    uint64_t cost = static_cast<uint64_t>(passes) *
                    ((4 * size) + (static_cast<uint64_t>(1) << bits));
    if (cost < best_cost) {
      best = {passes, bits};
      best_cost = cost;
    }
  }
  return best;
}

}  // namespace internal

// The number of counts RadixSort() needs for `size` keys of `key_bits` bits.
inline size_t RadixSortCountsSize(uint32_t key_bits, size_t size) {
  internal::RadixDigits digits = internal::GetRadixDigits(key_bits, size);
  return static_cast<size_t>(digits.passes) << digits.bits;
}

// Sorts [begin, end) by `key(element)`, a uint64_t, with a stable Least
// Significant Digit (LSD) radix sort. Only the low `key_bits` bits of the keys
// are sorted on: any bits above them must be the same in every key.
//
// Each pass is a stable counting sort on one digit, from `source` into the
// other buffer, and the two buffers then swap roles. The counts of every digit
// are taken in one read of the input, and a pass is skipped when every element
// has the same digit, as it would not move anything.
//
// @param scratch_begin A buffer of at least `end - begin` elements.
// @param counts Reusable buffer with at least
// `RadixSortCountsSize(key_bits, end - begin)` elements.
// @return Whichever of `begin` and `scratch_begin` holds the sorted elements.
template <typename T, typename Key>
T* RadixSort(T* begin,
             T* end,
             T* scratch_begin,
             uint32_t* counts,
             uint32_t key_bits,
             Key key) {
  static_assert(std::is_trivially_copyable_v<T>,
                "T must be trivially copyable for radix sort to work.");
  auto n = static_cast<size_t>(end - begin);
  if (n <= 1 || key_bits == 0) {
    return begin;
  }
  internal::RadixDigits digits = internal::GetRadixDigits(key_bits, n);
  size_t buckets = static_cast<size_t>(1) << digits.bits;
  uint64_t mask = buckets - 1;

  // 1. Count frequencies for every digit in a single read of the input. Each
  // digit has its own table of `buckets` counts. Sorting changes the order of
  // elements but not these frequencies, so later passes can reuse them.
  memset(counts, 0, digits.passes * buckets * sizeof(uint32_t));
  for (T* it = begin; it != end; ++it) {
    uint64_t k = key(*it);
    for (uint32_t p = 0; p < digits.passes; ++p) {
      ++counts[(p * buckets) + ((k >> (p * digits.bits)) & mask)];
    }
  }
  T* source = begin;
  T* dest = scratch_begin;
  // Process digits from least to most significant. Each pass must be stable
  // to preserve the ordering established by the less significant digits.
  for (uint32_t p = 0; p < digits.passes; ++p) {
    uint32_t* count = counts + (p * buckets);
    uint32_t shift = p * digits.bits;
    // If every element is in the same bucket, this pass cannot change the
    // order. Leave the data in `source` without swapping buffers.
    if (count[(key(*source) >> shift) & mask] == n) {
      continue;
    }
    // 2. Convert frequencies into the starting output position of each bucket.
    uint32_t total = 0;
    for (size_t d = 0; d < buckets; ++d) {
      uint32_t c = count[d];
      count[d] = total;
      total += c;
    }
    // 3. Distribute elements in source order. Advancing each bucket's position
    // after every write preserves the relative order of equal digits.
    for (T* it = source; it != source + n; ++it) {
      dest[count[(key(*it) >> shift) & mask]++] = *it;
    }
    // The next pass reads this pass's output, avoiding a copy back to `begin`.
    std::swap(source, dest);
  }
  return source;
}

namespace internal {

// Whether radix sorting `size` keys of `key_bits` bits is estimated to cost
// less than comparing them. Each pass of RadixSort() reads and writes every
// element, and clears and sums its counts.
inline bool RadixSortIsCheaper(size_t size, uint32_t key_bits) {
  RadixDigits digits = GetRadixDigits(key_bits, size);
  uint64_t radix = static_cast<uint64_t>(digits.passes) *
                   ((static_cast<uint64_t>(3) * size) +
                    (static_cast<uint64_t>(1) << digits.bits));
  uint64_t log2_size = 64 - base::CountLeadZeros64(size);
  return radix < static_cast<uint64_t>(2) * size * log2_size;
}

}  // namespace internal

// Stably sorts [begin, end) by `key(element)`, a uint64_t, of which only the
// low `key_bits` bits may differ between elements. Uses RadixSort() when that
// is estimated to be cheaper, as for large inputs, and otherwise std::sort(),
// breaking ties on `position(element)`, the element's position in the input.
//
// @param scratch_begin A buffer of at least `end - begin` elements.
// @return Whichever of `begin` and `scratch_begin` holds the sorted elements.
template <typename T, typename Key, typename Position>
T* StableSortByKey(T* begin,
                   T* end,
                   T* scratch_begin,
                   uint32_t key_bits,
                   Key key,
                   Position position) {
  auto size = static_cast<size_t>(end - begin);
  if (internal::RadixSortIsCheaper(size, key_bits)) {
    std::unique_ptr<uint32_t[]> counts(
        new uint32_t[RadixSortCountsSize(key_bits, size)]);
    return RadixSort(begin, end, scratch_begin, counts.get(), key_bits, key);
  }
  std::sort(begin, end, [&key, &position](const T& a, const T& b) {
    uint64_t key_a = key(a);
    uint64_t key_b = key(b);
    return key_a != key_b ? key_a < key_b : position(a) < position(b);
  });
  return begin;
}

// Sorts a collection of elements using a Most Significant Digit (MSD) radix
// sort. This implementation is particularly well-suited for sorting elements
// with variable-length string keys.
//
// The algorithm operates by partitioning the data into buckets based on the
// most significant character of their keys. It then recursively sorts each
// bucket based on the next character. This "divide and conquer" approach is
// managed iteratively using an explicit stack (`WorkItem`) to avoid deep
// recursion and potential stack overflow.
//
// For performance, a cutoff (`kMsdInsertionSortCutoff`) is used. When a bucket
// becomes smaller than this threshold, the algorithm switches to a standard
// comparison-based sort (`std::sort`), which is more efficient for small
// collections.
//
// To sort a sub-array, the data is first copied into a scratch buffer.
// The elements are then written from the scratch buffer back into the original
// buffer in sorted order. This ensures that the `begin` buffer always
// contains the partially sorted data and the final result is in-place.
//
// Stability: This sort is NOT stable. The relative order of elements with
// equal keys is not guaranteed to be preserved. This is because the
// partitioning process can reorder equal elements, and the `std::sort` used
// for small buckets is also not guaranteed to be stable.
//
// @param begin Pointer to the first element of the collection to be sorted.
// @param end Pointer to one past the last element of the collection.
// @param scratch_begin Pointer to a buffer of at least `end - begin` elements,
// used as scratch space.
// @param string_extractor A functor that takes an element and returns a
// `std::string_view` representing the sort key.
// @return A pointer to the beginning of the sorted collection, which will
// always be `begin`.
template <typename T, typename StringExtractor>
T* MsdRadixSort(T* begin,
                T* end,
                T* scratch_begin,
                StringExtractor string_extractor) {
  static_assert(std::is_trivially_copyable_v<T>,
                "T must be trivially copyable for radix sort to work.");

  if (end - begin <= 1) {
    return begin;
  }

  // WorkItem represents a sub-array to be sorted.
  struct WorkItem {
    T* begin;
    T* end;
    size_t depth;  // Current character index to sort by.
  };
  std::vector<WorkItem> stack;
  stack.push_back({begin, end, 0});

  while (!stack.empty()) {
    WorkItem item = stack.back();
    stack.pop_back();

    size_t item_size = static_cast<size_t>(item.end - item.begin);

    // A cutoff for switching to std::sort; for very small counts, insertion
    // sort will be optimal (that's what std::sort will do under the hood).
    //
    // Empirically chosen by changing the value and measuring the impact on
    // the benchmark `BM_DataframeSortMsdRadix`.
    static constexpr size_t kStdSortCutoff = 24;
    if (item_size <= kStdSortCutoff) {
      std::sort(item.begin, item.end, [&](const T& a, const T& b) {
        return string_extractor(a).substr(item.depth) <
               string_extractor(b).substr(item.depth);
      });
      continue;
    }

    // --- Distribution pass (similar to counting sort) ---
    // Copy the current chunk to the scratch buffer to read from it.
    ptrdiff_t item_offset = item.begin - begin;
    T* scratch_chunk_begin = scratch_begin + item_offset;
    memcpy(scratch_chunk_begin, item.begin, item_size * sizeof(T));

    // 1. Count frequencies of each character at the current depth.
    // Index 0 is for strings that are shorter than the current depth.
    size_t counts[257] = {};
    for (T* it = scratch_chunk_begin; it != scratch_chunk_begin + item_size;
         ++it) {
      std::string_view key = string_extractor(*it);
      counts[item.depth < key.size() ? uint8_t(key[item.depth]) + 1 : 0]++;
    }

    // 2. Calculate cumulative counts to determine bucket boundaries.
    size_t total = 0;
    for (size_t& count : counts) {
      size_t old_count = count;
      count = total;
      total += old_count;
    }

    // 3. Place elements from scratch back into the main buffer based on
    // character.
    for (T* it = scratch_chunk_begin; it != scratch_chunk_begin + item_size;
         ++it) {
      std::string_view key = string_extractor(*it);
      size_t& pos =
          counts[item.depth < key.size() ? uint8_t(key[item.depth]) + 1 : 0];
      item.begin[pos++] = *it;
    }

    // Push new work items for each bucket onto the stack for the next level.
    // We iterate backwards to process buckets for smaller characters first.
    for (ptrdiff_t i = 255; i >= 0; --i) {
      T* bucket_begin = item.begin + counts[i];
      T* bucket_end = item.begin + counts[i + 1];
      if (bucket_end - bucket_begin > 1) {
        stack.push_back({bucket_begin, bucket_end, item.depth + 1});
      }
    }
  }
  return begin;
}

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_SORT_H_
