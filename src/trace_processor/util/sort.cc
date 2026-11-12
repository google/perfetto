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

#include "src/trace_processor/util/sort.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

namespace perfetto::trace_processor {

namespace {

inline void SortByComparison(KeyedIndex* begin, KeyedIndex* end) {
  std::sort(begin, end, [](const KeyedIndex& a, const KeyedIndex& b) {
    return a.key < b.key || (a.key == b.key && a.index < b.index);
  });
}

}  // namespace

void sort_internal::CompareSortKeyedIndices(KeyedIndex* begin,
                                            KeyedIndex* end) {
  SortByComparison(begin, end);
}

void sort_internal::SortKeyedIndices(KeyedIndex* begin, KeyedIndex* end) {
  auto size = static_cast<size_t>(end - begin);
  // Below this, comparing is no slower than radix sorting and needs no scan.
  constexpr size_t kMinRadixSortSize = 2048;
  if (size < kMinRadixSortSize) {
    SortByComparison(begin, end);
    return;
  }
  // Stops at the first key less than the one before, at once for most input.
  size_t ascending = 1;
  while (ascending < size && begin[ascending].key >= begin[ascending - 1].key) {
    ++ascending;
  }
  if (ascending == size) {
    // Already in order: the items are in index order, so ties are too.
    return;
  }
  // Comparison sorts nearly sorted input faster than radix passes do. Pairs
  // at scattered positions estimate how sorted it is without reading it all;
  // evenly spaced ones could line up with a pattern in the input.
  constexpr size_t kSamples = 256;
  size_t descents = 0;
  for (uint64_t s = 1; s <= kSamples; ++s) {
    size_t i =
        1 + static_cast<size_t>((s * 0x9E3779B97F4A7C15ull) % (size - 1));
    descents += begin[i].key < begin[i - 1].key;
  }
  if (descents < kSamples / 16) {
    SortByComparison(begin, end);
    return;
  }
  // Only input which may be radix sorted pays for reading every key again, to
  // find the bits which differ: radix passes over just those are fewer.
  uint64_t differing = 0;
  for (const KeyedIndex* it = begin + 1; it != end; ++it) {
    differing |= it->key ^ begin->key;
  }
  uint32_t key_bits = 64 - base::CountLeadZeros64(differing);
  if (!RadixSortIsCheaper(size, key_bits)) {
    SortByComparison(begin, end);
    return;
  }
  std::unique_ptr<KeyedIndex[]> scratch(new KeyedIndex[size]);
  KeyedIndex* sorted =
      StableSortKeyedIndices(begin, end, scratch.get(), key_bits);
  if (sorted != begin) {
    memcpy(begin, sorted, size * sizeof(KeyedIndex));
  }
}

KeyedIndex* StableSortKeyedIndices(KeyedIndex* begin,
                                   KeyedIndex* end,
                                   KeyedIndex* scratch_begin,
                                   uint32_t key_bits) {
  auto size = static_cast<size_t>(end - begin);
  if (!sort_internal::RadixSortIsCheaper(size, key_bits)) {
    sort_internal::CompareSortKeyedIndices(begin, end);
    return begin;
  }
  std::unique_ptr<uint32_t[]> counts(
      new uint32_t[RadixSortCountsSize(key_bits, size)]);
  return RadixSort(begin, end, scratch_begin, counts.get(), key_bits,
                   [](const KeyedIndex& k) { return k.key; });
}

}  // namespace perfetto::trace_processor
