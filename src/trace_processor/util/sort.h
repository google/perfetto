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

#ifndef SRC_TRACE_PROCESSOR_UTIL_SORT_H_
#define SRC_TRACE_PROCESSOR_UTIL_SORT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/bits.h"
#include "perfetto/ext/base/small_vector.h"

namespace perfetto::trace_processor {

// An element's key and position. The shared sorts below order these by key,
// then position, so each of them is stable.
struct KeyedIndex {
  uint64_t key;
  uint32_t index;
  // Carried along but never compared. It fills what would be padding.
  uint32_t value;
};
namespace sort_internal {

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

}  // namespace sort_internal

// The number of counts RadixSort() needs for `size` keys of `key_bits` bits.
inline size_t RadixSortCountsSize(uint32_t key_bits, size_t size) {
  sort_internal::RadixDigits digits =
      sort_internal::GetRadixDigits(key_bits, size);
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
  sort_internal::RadixDigits digits =
      sort_internal::GetRadixDigits(key_bits, n);
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

namespace sort_internal {

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

}  // namespace sort_internal

// Stably sorts [begin, end) by key, ties by index, where only the low
// `key_bits` bits of the keys may differ. Radix sorts when that is estimated to
// be cheaper, as for large inputs, and otherwise uses the comparison sort the
// sorts by key below share, so its callers add no sorting code of their own.
//
// @param scratch_begin A buffer of at least `end - begin` elements.
// @return Whichever of `begin` and `scratch_begin` holds the sorted elements.
KeyedIndex* StableSortKeyedIndices(KeyedIndex* begin,
                                   KeyedIndex* end,
                                   KeyedIndex* scratch_begin,
                                   uint32_t key_bits);

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

// Sorts by key.
//
// Replacements for std::sort() and std::stable_sort(), which are compiled for
// every element type and comparator, at 3-4KB each. These sort by an integer
// key through one sort compiled once, so each caller adds only a few hundred
// bytes. Many elements in no particular order are radix sorted, faster than
// std::sort(); few or nearly sorted elements are compared, up to 2x slower
// than std::sort() on small elements. They need 16 bytes per element of extra
// memory, only heap allocated above 64 elements, and 16 more when radix
// sorting.

namespace sort_internal {

using KeyedIndices = base::SmallVector<KeyedIndex, 64>;

// Sorts by key, then index, by comparison.
void CompareSortKeyedIndices(KeyedIndex* begin, KeyedIndex* end);

// Sorts by key, then index: by radix when that is estimated to be cheaper,
// as for many elements in no particular order, and otherwise by comparison.
void SortKeyedIndices(KeyedIndex* begin, KeyedIndex* end);

// Maps `key` to a uint64_t with the same order.
template <typename K>
uint64_t ToOrderedKey(K key) {
  static_assert(std::is_integral_v<K>, "Sorts by key need integer keys");
  if constexpr (std::is_signed_v<K>) {
    return static_cast<uint64_t>(static_cast<int64_t>(key)) ^ (1ull << 63);
  } else {
    return static_cast<uint64_t>(key);
  }
}

// Sorts [begin, end) by key, ties in their original order.
template <typename It, typename Key>
void SortByKey(It begin, It end, Key key) {
  using T = typename std::iterator_traits<It>::value_type;
  PERFETTO_DCHECK(static_cast<uint64_t>(end - begin) <=
                  std::numeric_limits<uint32_t>::max());
  auto size = static_cast<uint32_t>(end - begin);
  if (size < 2) {
    return;
  }
  KeyedIndices keys;
  keys.reserve(size);
  for (uint32_t i = 0; i < size; ++i) {
    keys.emplace_back(KeyedIndex{ToOrderedKey(key(begin[i])), i, 0});
  }
  SortKeyedIndices(keys.data(), keys.data() + size);
  // Move each element to its sorted position by following the permutation's
  // cycles, so only one element is ever held outside the range.
  for (uint32_t i = 0; i < size; ++i) {
    if (keys[i].index == i) {
      continue;
    }
    T held = std::move(begin[i]);
    uint32_t j = i;
    for (uint32_t k = keys[j].index; k != i; k = keys[j].index) {
      begin[j] = std::move(begin[k]);
      keys[j].index = j;
      j = k;
    }
    begin[j] = std::move(held);
    keys[j].index = j;
  }
}

}  // namespace sort_internal

// Sorts [begin, end) by the integer `key(element)`, ascending. Elements with
// equal keys end up in an unspecified order.
template <typename It, typename Key>
void SortByKey(It begin, It end, Key key) {
  sort_internal::SortByKey(begin, end, std::move(key));
}

// As SortByKey(), but descending.
template <typename It, typename Key>
void SortByKeyDescending(It begin, It end, Key key) {
  sort_internal::SortByKey(begin, end, [&key](const auto& element) {
    return ~sort_internal::ToOrderedKey(key(element));
  });
}

// As SortByKey(), but elements with equal keys keep their order.
template <typename It, typename Key>
void StableSortByKey(It begin, It end, Key key) {
  sort_internal::SortByKey(begin, end, std::move(key));
}

// As StableSortByKey(), but descending.
template <typename It, typename Key>
void StableSortByKeyDescending(It begin, It end, Key key) {
  sort_internal::SortByKey(begin, end, [&key](const auto& element) {
    return ~sort_internal::ToOrderedKey(key(element));
  });
}

// Stably sorts the elements of [first_begin, first_end) followed by those of
// [second_begin, second_end), as one range, by the integer `key(element)`:
// a range a ring buffer splits in two, or one range when the second is empty.
// Many elements are radix sorted on just the bits in which their keys differ,
// and the rest compared. Unlike StableSortByKey(), it moves the elements
// themselves rather than their indices and is compiled for each element type
// and key: for hot paths sorting many small elements which carry their key.
template <typename T, typename Key>
void StableSortElementsByKey(T* first_begin,
                             T* first_end,
                             T* second_begin,
                             T* second_end,
                             Key key) {
  static_assert(std::is_trivially_copyable_v<T>,
                "T must be trivially copyable to be sorted in place.");
  auto first_size = static_cast<size_t>(first_end - first_begin);
  auto second_size = static_cast<size_t>(second_end - second_begin);
  size_t size = first_size + second_size;
  if (size < 2) {
    return;
  }
  auto ordered = [&key](const T& e) {
    return sort_internal::ToOrderedKey(key(e));
  };
  uint64_t base = ordered(*(first_size ? first_begin : second_begin));
  uint64_t differing = 0;
  for (const T* it = first_begin; it != first_end; ++it) {
    differing |= ordered(*it) ^ base;
  }
  for (const T* it = second_begin; it != second_end; ++it) {
    differing |= ordered(*it) ^ base;
  }
  uint32_t key_bits = 64 - base::CountLeadZeros64(differing);
  bool radix = sort_internal::RadixSortIsCheaper(size, key_bits);
  auto less = [&key](const T& a, const T& b) { return key(a) < key(b); };
  if (!radix && second_size == 0) {
    std::stable_sort(first_begin, first_end, less);
    return;
  }
  // A range in two runs is gathered into one to be sorted, and put back.
  std::unique_ptr<T[]> gathered;
  T* data = first_begin;
  if (second_size) {
    gathered.reset(new T[size]);
    data = gathered.get();
    memcpy(data, first_begin, first_size * sizeof(T));
    memcpy(data + first_size, second_begin, second_size * sizeof(T));
  }
  T* sorted = data;
  std::unique_ptr<T[]> scratch;
  if (radix) {
    scratch.reset(new T[size]);
    std::unique_ptr<uint32_t[]> counts(
        new uint32_t[RadixSortCountsSize(key_bits, size)]);
    sorted = RadixSort(data, data + size, scratch.get(), counts.get(), key_bits,
                       ordered);
  } else {
    std::stable_sort(data, data + size, less);
  }
  if (sorted != first_begin) {
    memcpy(first_begin, sorted, first_size * sizeof(T));
    memcpy(second_begin, sorted + first_size, second_size * sizeof(T));
  }
}

// As above, for one range.
template <typename T, typename Key>
void StableSortElementsByKey(T* begin, T* end, Key key) {
  StableSortElementsByKey(begin, end, end, end, std::move(key));
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_SORT_H_
