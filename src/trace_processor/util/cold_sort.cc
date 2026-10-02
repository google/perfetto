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
#include <cstdint>

namespace perfetto::trace_processor::cold_sort_internal {

void SortIndices(uint32_t* begin,
                 uint32_t* end,
                 IndexLess less,
                 const void* context) {
  std::stable_sort(begin, end, [less, context](uint32_t a, uint32_t b) {
    return less(context, a, b);
  });
}

}  // namespace perfetto::trace_processor::cold_sort_internal
