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

#include "src/trace_processor/core/util/sort.h"

#include <cstdint>
#include <memory>

#include "src/trace_processor/util/cold_sort.h"

namespace perfetto::trace_processor::core {

KeyedIndex* StableSortKeyedIndices(KeyedIndex* begin,
                                   KeyedIndex* end,
                                   KeyedIndex* scratch_begin,
                                   uint32_t key_bits) {
  auto size = static_cast<size_t>(end - begin);
  if (!internal::RadixSortIsCheaper(size, key_bits)) {
    cold_sort_internal::SortKeyedIndices(begin, end);
    return begin;
  }
  std::unique_ptr<uint32_t[]> counts(
      new uint32_t[RadixSortCountsSize(key_bits, size)]);
  return RadixSort(begin, end, scratch_begin, counts.get(), key_bits,
                   [](const KeyedIndex& k) { return k.key; });
}

}  // namespace perfetto::trace_processor::core
