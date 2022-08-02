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

#include "src/trace_processor/dynamic/experimental_counter_dur_generator.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_COUNTER_DUR_TABLE_DEF(NAME, PARENT, C)      \
  NAME(ExperimentalCounterDurTable, "experimental_counter_dur") \
  PARENT(PERFETTO_TP_COUNTER_TABLE_DEF, C)                      \
  C(int64_t, dur)                                               \
  C(double, delta)

PERFETTO_TP_TABLE(PERFETTO_TP_COUNTER_DUR_TABLE_DEF);

ExperimentalCounterDurTable::~ExperimentalCounterDurTable() = default;

}  // namespace tables

ExperimentalCounterDurGenerator::ExperimentalCounterDurGenerator(
    const tables::CounterTable& table)
    : counter_table_(&table) {}
ExperimentalCounterDurGenerator::~ExperimentalCounterDurGenerator() = default;

Table::Schema ExperimentalCounterDurGenerator::CreateSchema() {
  return tables::ExperimentalCounterDurTable::Schema();
}

std::string ExperimentalCounterDurGenerator::TableName() {
  return tables::ExperimentalCounterDurTable::Name();
}

uint32_t ExperimentalCounterDurGenerator::EstimateRowCount() {
  return counter_table_->row_count();
}

base::Status ExperimentalCounterDurGenerator::ValidateConstraints(
    const QueryConstraints&) {
  return base::OkStatus();
}

base::Status ExperimentalCounterDurGenerator::ComputeTable(
    const std::vector<Constraint>&,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  if (!counter_dur_table_) {
    counter_dur_table_ = tables::ExperimentalCounterDurTable::ExtendParent(
        *counter_table_, ComputeDurColumn(*counter_table_),
        ComputeDeltaColumn(*counter_table_));
  }
  table_return.reset(new Table(counter_dur_table_->Copy()));
  return base::OkStatus();
}

// static
ColumnStorage<int64_t> ExperimentalCounterDurGenerator::ComputeDurColumn(
    const Table& table) {
  // Keep track of the last seen row for each track id.
  std::unordered_map<TrackId, uint32_t> last_row_for_track_id;
  ColumnStorage<int64_t> dur;

  const auto* ts_col =
      TypedColumn<int64_t>::FromColumn(table.GetColumnByName("ts"));
  const auto* track_id_col =
      TypedColumn<tables::CounterTrackTable::Id>::FromColumn(
          table.GetColumnByName("track_id"));

  for (uint32_t i = 0; i < table.row_count(); ++i) {
    // Check if we already have a previous row for the current track id.
    TrackId track_id = (*track_id_col)[i];
    auto it = last_row_for_track_id.find(track_id);
    if (it == last_row_for_track_id.end()) {
      // This means we don't have any row - start tracking this row for the
      // future.
      last_row_for_track_id.emplace(track_id, i);
    } else {
      // This means we have an previous row for the current track id. Update
      // the duration of the previous row to be up to the current ts.
      uint32_t old_row = it->second;
      it->second = i;
      dur.Set(old_row, (*ts_col)[i] - (*ts_col)[old_row]);
    }
    // Append -1 to mark this event as not having been finished. On a later
    // row, we may set this to have the correct value.
    dur.Append(-1);
  }
  return dur;
}

// static
ColumnStorage<double> ExperimentalCounterDurGenerator::ComputeDeltaColumn(
    const Table& table) {
  // Keep track of the last seen row for each track id.
  std::unordered_map<TrackId, uint32_t> last_row_for_track_id;
  ColumnStorage<double> delta;

  const auto* value_col =
      TypedColumn<double>::FromColumn(table.GetColumnByName("value"));
  const auto* track_id_col =
      TypedColumn<tables::CounterTrackTable::Id>::FromColumn(
          table.GetColumnByName("track_id"));

  for (uint32_t i = 0; i < table.row_count(); ++i) {
    // Check if we already have a previous row for the current track id.
    TrackId track_id = (*track_id_col)[i];
    auto it = last_row_for_track_id.find(track_id);
    if (it == last_row_for_track_id.end()) {
      // This means we don't have any row - start tracking this row for the
      // future.
      last_row_for_track_id.emplace(track_id, i);
    } else {
      // This means we have an previous row for the current track id. Update
      // the duration of the previous row to be up to the current ts.
      uint32_t old_row = it->second;
      it->second = i;
      delta.Set(old_row, (*value_col)[i] - (*value_col)[old_row]);
    }
    delta.Append(0);
  }
  return delta;
}

}  // namespace trace_processor
}  // namespace perfetto
