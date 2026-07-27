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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_OPS_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_OPS_H_

#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::ops {

// Estimates the number of distinct values using a bounded strided sample.
template <typename T>
uint32_t EstimateDistinctCount(base::FlatHashMap<int64_t, uint32_t>* counts,
                               Span<const T> values);

// Removes duplicate fixed-width rows and compacts |indices| in place. Rows in
// |row_layout| are parallel to indices rather than addressed by their values.
void DistinctRows(Span<const uint8_t> row_layout,
                  uint32_t row_stride,
                  Span<uint32_t>* indices);

// Stably sorts indices by fixed-width rows in |row_layout|.
void SortRowLayout(Span<const uint8_t> row_layout,
                   uint32_t row_stride,
                   Span<uint32_t>* indices);

#define PERFETTO_DECLARE_DISTINCT(type)                 \
  extern template uint32_t EstimateDistinctCount<type>( \
      base::FlatHashMap<int64_t, uint32_t>*, Span<const type>);
PERFETTO_DECLARE_DISTINCT(uint32_t)
PERFETTO_DECLARE_DISTINCT(int32_t)
PERFETTO_DECLARE_DISTINCT(int64_t)
PERFETTO_DECLARE_DISTINCT(StringPool::Id)
#undef PERFETTO_DECLARE_DISTINCT

}  // namespace perfetto::trace_processor::core::ops

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_OPS_H_
