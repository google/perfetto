// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/trace_processor/perfetto_sql/intrinsics/functions/perf_counter.h"

#include <cstdint>
#include <optional>

#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/counter_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"

namespace perfetto::trace_processor {

// static
void PerfCounterForSampleFunction::Step(sqlite3_context* ctx,
                                        int,
                                        sqlite3_value** argv) {
  sqlite::Type sample_id_type = sqlite::value::Type(argv[0]);
  sqlite::Type counter_name_type = sqlite::value::Type(argv[1]);

  // If the sample_id is null, return null.
  if (sample_id_type == sqlite::Type::kNull) {
    return;
  }

  if (sample_id_type != sqlite::Type::kInteger) {
    return sqlite::result::Error(ctx,
                                 "__intrinsic_perf_counter_for_sample: 1st "
                                 "argument should be sample id");
  }

  if (counter_name_type != sqlite::Type::kText) {
    return sqlite::result::Error(
        ctx,
        "__intrinsic_perf_counter_for_sample: 2nd argument should be counter "
        "name");
  }

  auto sample_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
  const char* counter_name = sqlite::value::Text(argv[1]);

  auto* user_data = GetUserData(ctx);
  auto* storage = user_data->storage;

  // Look up the sample to get counter_set_id.
  const auto& perf_sample_table = storage->perf_sample_table();
  if (sample_id >= perf_sample_table.row_count()) {
    // Invalid sample ID.
    return;
  }

  auto counter_set_id = perf_sample_table[sample_id].counter_set_id();
  if (!counter_set_id.has_value()) {
    // No counter set for this sample.
    return;
  }

  // Iterate through the counter set to find the counter with matching name.
  const auto& perf_counter_set_table = storage->perf_counter_set_table();
  const auto& counter_table = storage->counter_table();
  const auto& track_table = storage->track_table();

  for (auto it = perf_counter_set_table.IterateRows(); it; ++it) {
    if (it.perf_counter_set_id() != *counter_set_id) {
      continue;
    }

    // Get the counter row.
    CounterId counter_id = it.counter_id();
    if (counter_id.value >= counter_table.row_count()) {
      continue;
    }

    // Get the track for this counter.
    TrackId track_id = counter_table[counter_id.value].track_id();
    if (track_id.value >= track_table.row_count()) {
      continue;
    }

    // Check if the track name matches.
    auto track_name_str =
        storage->GetString(track_table[track_id.value].name());
    if (track_name_str == counter_name) {
      // Found matching counter, return its value.
      double value = counter_table[counter_id.value].value();
      return sqlite::result::Double(ctx, value);
    }
  }

  // No matching counter found.
  return;
}

}  // namespace perfetto::trace_processor
