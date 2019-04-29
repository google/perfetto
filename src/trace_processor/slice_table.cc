/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/slice_table.h"

#include "src/trace_processor/storage_columns.h"

namespace perfetto {
namespace trace_processor {

SliceTable::SliceTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void SliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SliceTable>(db, storage, "internal_slice");
}

StorageSchema SliceTable::CreateStorageSchema() {
  const auto& slices = storage_->nestable_slices();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("slice_id", RowAccessor())
      .AddOrderedNumericColumn("ts", &slices.start_ns())
      .AddNumericColumn("dur", &slices.durations())
      .AddNumericColumn("ref", &slices.refs())
      .AddStringColumn("ref_type", &slices.types(), &GetRefTypeStringMap())
      .AddStringColumn("cat", &slices.cats(), &storage_->string_pool())
      .AddStringColumn("name", &slices.names(), &storage_->string_pool())
      .AddNumericColumn("depth", &slices.depths())
      .AddNumericColumn("stack_id", &slices.stack_ids())
      .AddNumericColumn("parent_stack_id", &slices.parent_stack_ids())
      .Build({"slice_id"});
}

uint32_t SliceTable::RowCount() {
  return static_cast<uint32_t>(storage_->nestable_slices().slice_count());
}

int SliceTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost = EstimateCost(qc);

  // Only the string columns are handled by SQLite
  info->order_by_consumed = true;
  size_t name_index = schema().ColumnIndexFromName("name");
  size_t cat_index = schema().ColumnIndexFromName("cat");
  size_t ref_type_index = schema().ColumnIndexFromName("ref_type");
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    auto col = static_cast<size_t>(qc.constraints()[i].iColumn);
    info->omit[i] =
        col != name_index && col != cat_index && col != ref_type_index;
  }
  return SQLITE_OK;
}

uint32_t SliceTable::EstimateCost(const QueryConstraints& qc) {
  // slice_id is row index, so we can filter efficiently for equality.
  if (HasEqConstraint(qc, "slice_id"))
    return 1;

  auto eq_ts = HasEqConstraint(qc, "ts");
  auto eq_ref = HasEqConstraint(qc, "ref");
  auto eq_ref_type = HasEqConstraint(qc, "ref_type");
  auto eq_depth = HasEqConstraint(qc, "depth");
  auto eq_name = HasEqConstraint(qc, "name");

  // ref + ref_type + ts + depth is a unique key. others are estimates.
  if (eq_ref && eq_ref_type && eq_ts && eq_depth)
    return 1;
  else if (eq_ref && eq_ref_type && eq_ts)
    return 10;
  else if (eq_ts && eq_name)
    return 10;
  else if (eq_ts || eq_name)
    return 100;
  return RowCount();
}

}  // namespace trace_processor
}  // namespace perfetto
