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

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <numeric>
#include <utility>
#include <vector>

// Replacements for std::sort() and std::stable_sort() for code which is not
// hot. Those are compiled for every element type and comparator, at 2-4KB each.
// These share one sort compiled once, which orders indices through a function
// pointer, so each caller adds only a few hundred bytes. They are 1.3-7x
// slower, so they are not for hot paths.

namespace perfetto::trace_processor {
namespace cold_sort_internal {

using IndexLess = bool (*)(const void* context, uint32_t a, uint32_t b);

// Stably sorts [begin, end) by `less`.
void SortIndices(uint32_t* begin,
                 uint32_t* end,
                 IndexLess less,
                 const void* context);

}  // namespace cold_sort_internal

template <typename It, typename Less>
void ColdStableSort(It begin, It end, Less less) {
  using T = typename std::iterator_traits<It>::value_type;
  auto size = static_cast<uint32_t>(end - begin);
  if (size < 2) {
    return;
  }
  std::vector<uint32_t> order(size);
  std::iota(order.begin(), order.end(), 0u);
  struct Context {
    It begin;
    const Less* less;
  } context{begin, &less};
  cold_sort_internal::SortIndices(
      order.data(), order.data() + size,
      [](const void* c, uint32_t a, uint32_t b) {
        const auto* ctx = static_cast<const Context*>(c);
        return (*ctx->less)(ctx->begin[a], ctx->begin[b]);
      },
      &context);
  std::vector<T> sorted;
  sorted.reserve(size);
  for (uint32_t i : order) {
    sorted.push_back(std::move(begin[i]));
  }
  std::move(sorted.begin(), sorted.end(), begin);
}

template <typename It>
void ColdStableSort(It begin, It end) {
  ColdStableSort(begin, end, std::less<>());
}

// Also stable, as it shares ColdStableSort()'s code.
template <typename It, typename Less>
void ColdSort(It begin, It end, Less less) {
  ColdStableSort(begin, end, std::move(less));
}

template <typename It>
void ColdSort(It begin, It end) {
  ColdStableSort(begin, end, std::less<>());
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_COLD_SORT_H_
