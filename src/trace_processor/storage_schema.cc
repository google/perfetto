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

#include "src/trace_processor/storage_schema.h"

#include "src/trace_processor/row_iterators.h"

namespace perfetto {
namespace trace_processor {

StorageSchema::StorageSchema() = default;
StorageSchema::StorageSchema(std::vector<std::unique_ptr<Column>> columns)
    : columns_(std::move(columns)) {}

Table::Schema StorageSchema::ToTableSchema(std::vector<std::string> pkeys) {
  std::vector<Table::Column> columns;
  size_t i = 0;
  for (const auto& col : columns_)
    columns.emplace_back(i++, col->name(), col->GetType(), col->hidden());

  std::vector<size_t> primary_keys;
  for (const auto& p_key : pkeys)
    primary_keys.emplace_back(ColumnIndexFromName(p_key));
  return Table::Schema(std::move(columns), std::move(primary_keys));
}

size_t StorageSchema::ColumnIndexFromName(const std::string& name) {
  auto p = [name](const std::unique_ptr<Column>& col) {
    return name == col->name();
  };
  auto it = std::find_if(columns_.begin(), columns_.end(), p);
  return static_cast<size_t>(std::distance(columns_.begin(), it));
}

StorageSchema::Column::Column(std::string col_name, bool hidden)
    : col_name_(col_name), hidden_(hidden) {}
StorageSchema::Column::~Column() = default;

StorageSchema::TsEndColumn::TsEndColumn(std::string col_name,
                                        const std::deque<uint64_t>* ts_start,
                                        const std::deque<uint64_t>* dur)
    : Column(col_name, false), ts_start_(ts_start), dur_(dur) {}
StorageSchema::TsEndColumn::~TsEndColumn() = default;

void StorageSchema::TsEndColumn::ReportResult(sqlite3_context* ctx,
                                              uint32_t row) const {
  uint64_t add = (*ts_start_)[row] + (*dur_)[row];
  sqlite3_result_int64(ctx, static_cast<sqlite3_int64>(add));
}

StorageSchema::Column::Bounds StorageSchema::TsEndColumn::BoundFilter(
    int,
    sqlite3_value*) const {
  Bounds bounds;
  bounds.max_idx = static_cast<uint32_t>(ts_start_->size());
  return bounds;
}

void StorageSchema::TsEndColumn::Filter(int op,
                                        sqlite3_value* value,
                                        FilteredRowIndex* index) const {
  auto binary_op = sqlite_utils::GetPredicateForOp<uint64_t>(op);
  uint64_t extracted = sqlite_utils::ExtractSqliteValue<uint64_t>(value);
  index->FilterRows([this, &binary_op, extracted](uint32_t row) {
    uint64_t val = (*ts_start_)[row] + (*dur_)[row];
    return binary_op(val, extracted);
  });
}

StorageSchema::Column::Comparator StorageSchema::TsEndColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) {
      uint64_t a = (*ts_start_)[f] + (*dur_)[f];
      uint64_t b = (*ts_start_)[s] + (*dur_)[s];
      return sqlite_utils::CompareValuesDesc(a, b);
    };
  }
  return [this](uint32_t f, uint32_t s) {
    uint64_t a = (*ts_start_)[f] + (*dur_)[f];
    uint64_t b = (*ts_start_)[s] + (*dur_)[s];
    return sqlite_utils::CompareValuesAsc(a, b);
  };
}

StorageSchema::IdColumn::IdColumn(std::string column_name, TableId table_id)
    : Column(std::move(column_name), false), table_id_(table_id) {}
StorageSchema::IdColumn::~IdColumn() = default;

}  // namespace trace_processor
}  // namespace perfetto
