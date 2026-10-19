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

#include "src/trace_processor/util/cold_sort.h"

#include <algorithm>

namespace perfetto::trace_processor::cold_sort_internal {

void SortKeyedIndices(KeyedIndex* begin, KeyedIndex* end) {
  std::sort(begin, end, [](const KeyedIndex& a, const KeyedIndex& b) {
    return a.key < b.key || (a.key == b.key && a.index < b.index);
  });
}

}  // namespace perfetto::trace_processor::cold_sort_internal
