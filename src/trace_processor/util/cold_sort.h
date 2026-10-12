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

#ifndef SRC_TRACE_PROCESSOR_UTIL_COLD_SORT_H_
#define SRC_TRACE_PROCESSOR_UTIL_COLD_SORT_H_

#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"

// Replacements for std::sort() and std::stable_sort() for code which is not
// hot. Those are compiled for every element type and comparator, at 3-4KB
// each. These sort by an integer key through one sort compiled once, so each
// caller adds only a few hundred bytes. They are up to 2x slower, and need 16
// bytes per element of extra memory, only heap allocated above 64 elements.

namespace perfetto::trace_processor {
namespace cold_sort_internal {

struct KeyedIndex {
  uint64_t key;
  uint32_t index;
};
using KeyedIndices = base::SmallVector<KeyedIndex, 64>;

// Sorts by key, then index.
void SortKeyedIndices(KeyedIndex* begin, KeyedIndex* end);

// Maps `key` to a uint64_t with the same order.
template <typename K>
uint64_t ToOrderedKey(K key) {
  static_assert(std::is_integral_v<K>, "Cold sorts need integer keys");
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
  PERFETTO_DCHECK(end - begin <= std::numeric_limits<uint32_t>::max());
  auto size = static_cast<uint32_t>(end - begin);
  if (size < 2) {
    return;
  }
  KeyedIndices keys;
  for (uint32_t i = 0; i < size; ++i) {
    keys.emplace_back(KeyedIndex{ToOrderedKey(key(begin[i])), i});
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

}  // namespace cold_sort_internal

// Sorts [begin, end) by the integer `key(element)`, ascending. Elements with
// equal keys end up in an unspecified order.
template <typename It, typename Key>
void ColdSortByKey(It begin, It end, Key key) {
  cold_sort_internal::SortByKey(begin, end, std::move(key));
}

// As ColdSortByKey(), but descending.
template <typename It, typename Key>
void ColdSortByKeyDescending(It begin, It end, Key key) {
  cold_sort_internal::SortByKey(begin, end, [&key](const auto& element) {
    return ~cold_sort_internal::ToOrderedKey(key(element));
  });
}

// As ColdSortByKey(), but elements with equal keys keep their order.
template <typename It, typename Key>
void ColdStableSortByKey(It begin, It end, Key key) {
  cold_sort_internal::SortByKey(begin, end, std::move(key));
}

// As ColdStableSortByKey(), but descending.
template <typename It, typename Key>
void ColdStableSortByKeyDescending(It begin, It end, Key key) {
  cold_sort_internal::SortByKey(begin, end, [&key](const auto& element) {
    return ~cold_sort_internal::ToOrderedKey(key(element));
  });
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_COLD_SORT_H_
