/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_counter_dur.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/counter_tables_py.h"

namespace perfetto::trace_processor {
namespace tables {

ExperimentalCounterDurTable::~ExperimentalCounterDurTable() = default;

}  // namespace tables

ExperimentalCounterDur::ExperimentalCounterDur(
    const tables::CounterTable& table)
    : counter_table_(&table) {}
ExperimentalCounterDur::~ExperimentalCounterDur() = default;

Table::Schema ExperimentalCounterDur::CreateSchema() {
  return tables::ExperimentalCounterDurTable::ComputeStaticSchema();
}

std::string ExperimentalCounterDur::TableName() {
  return tables::ExperimentalCounterDurTable::Name();
}

uint32_t ExperimentalCounterDur::EstimateRowCount() {
  return counter_table_->row_count();
}

base::StatusOr<std::unique_ptr<Table>> ExperimentalCounterDur::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.empty());
  if (!counter_dur_table_) {
    counter_dur_table_ = tables::ExperimentalCounterDurTable::ExtendParent(
        *counter_table_, ComputeDurColumn(*counter_table_),
        ComputeDeltaColumn(*counter_table_));
  }
  return std::make_unique<Table>(counter_dur_table_->Copy());
}

// static
ColumnStorage<int64_t> ExperimentalCounterDur::ComputeDurColumn(
    const CounterTable& table) {
  // Keep track of the last seen row for each track id.
  std::unordered_map<TrackId, CounterTable::RowNumber> last_row_for_track_id;
  ColumnStorage<int64_t> dur;

  for (auto table_it = table.IterateRows(); table_it; ++table_it) {
    // Check if we already have a previous row for the current track id.
    TrackId track_id = table_it.track_id();
    auto it = last_row_for_track_id.find(track_id);
    if (it == last_row_for_track_id.end()) {
      // This means we don't have any row - start tracking this row for the
      // future.
      last_row_for_track_id.emplace(track_id, table_it.row_number());
    } else {
      // This means we have an previous row for the current track id. Update
      // the duration of the previous row to be up to the current ts.
      CounterTable::RowNumber old_row = it->second;
      it->second = table_it.row_number();
      dur.Set(old_row.row_number(),
              table_it.ts() - old_row.ToRowReference(table).ts());
    }
    // Append -1 to mark this event as not having been finished. On a later
    // row, we may set this to have the correct value.
    dur.Append(-1);
  }
  return dur;
}

// static
ColumnStorage<double> ExperimentalCounterDur::ComputeDeltaColumn(
    const CounterTable& table) {
  // Keep track of the last seen row for each track id.
  std::unordered_map<TrackId, CounterTable::RowNumber> last_row_for_track_id;
  ColumnStorage<double> delta;

  for (auto table_it = table.IterateRows(); table_it; ++table_it) {
    // Check if we already have a previous row for the current track id.
    TrackId track_id = table_it.track_id();
    auto it = last_row_for_track_id.find(track_id);
    if (it == last_row_for_track_id.end()) {
      // This means we don't have any row - start tracking this row for the
      // future.
      last_row_for_track_id.emplace(track_id, table_it.row_number());
    } else {
      // This means we have an previous row for the current track id. Update
      // the duration of the previous row to be up to the current ts.
      CounterTable::RowNumber old_row = it->second;
      it->second = table_it.row_number();
      delta.Set(old_row.row_number(),
                table_it.value() - old_row.ToRowReference(table).value());
    }
    delta.Append(0);
  }
  return delta;
}

}  // namespace perfetto::trace_processor
