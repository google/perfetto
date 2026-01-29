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

#include "src/trace_processor/importers/proto/perf_counter_set_tracker.h"

#include <cstdint>
#include <vector>

#include "src/trace_processor/tables/profiler_tables_py.h"

namespace perfetto::trace_processor {

PerfCounterSetTracker::PerfCounterSetTracker(TraceProcessorContext* context)
    : context_(context) {}

uint32_t PerfCounterSetTracker::AddCounterSet(
    const std::vector<CounterId>& counter_ids) {
  auto* table = context_->storage->mutable_perf_counter_set_table();

  // The set ID is the current row count (where the set starts)
  uint32_t set_id = static_cast<uint32_t>(table->row_count());

  // Insert a row for each counter in the set
  for (CounterId counter_id : counter_ids) {
    tables::PerfCounterSetTable::Row row;
    row.perf_counter_set_id = set_id;
    row.counter_id = counter_id;
    table->Insert(row);
  }

  return set_id;
}

}  // namespace perfetto::trace_processor
