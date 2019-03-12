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

#include "src/trace_processor/storage_columns.h"

namespace perfetto {
namespace trace_processor {

StorageColumn::StorageColumn(std::string col_name, bool hidden)
    : col_name_(col_name), hidden_(hidden) {}
StorageColumn::~StorageColumn() = default;

TsEndColumn::TsEndColumn(std::string col_name,
                         const std::deque<int64_t>* ts_start,
                         const std::deque<int64_t>* dur)
    : StorageColumn(col_name, false /* hidden */),
      ts_start_(ts_start),
      dur_(dur) {}
TsEndColumn::~TsEndColumn() = default;

void TsEndColumn::ReportResult(sqlite3_context* ctx, uint32_t row) const {
  int64_t add = (*ts_start_)[row] + (*dur_)[row];
  sqlite3_result_int64(ctx, static_cast<sqlite3_int64>(add));
}

TsEndColumn::Bounds TsEndColumn::BoundFilter(int, sqlite3_value*) const {
  Bounds bounds;
  bounds.max_idx = static_cast<uint32_t>(ts_start_->size());
  return bounds;
}

void TsEndColumn::Filter(int op,
                         sqlite3_value* value,
                         FilteredRowIndex* index) const {
  auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
  index->FilterRows([this, predicate](uint32_t row) PERFETTO_ALWAYS_INLINE {
    return predicate((*ts_start_)[row] + (*dur_)[row]);
  });
}

TsEndColumn::Comparator TsEndColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) {
      int64_t a = (*ts_start_)[f] + (*dur_)[f];
      int64_t b = (*ts_start_)[s] + (*dur_)[s];
      return sqlite_utils::CompareValuesDesc(a, b);
    };
  }
  return [this](uint32_t f, uint32_t s) {
    int64_t a = (*ts_start_)[f] + (*dur_)[f];
    int64_t b = (*ts_start_)[s] + (*dur_)[s];
    return sqlite_utils::CompareValuesAsc(a, b);
  };
}

IdColumn::IdColumn(std::string column_name, TableId table_id)
    : StorageColumn(std::move(column_name), false), table_id_(table_id) {}
IdColumn::~IdColumn() = default;

}  // namespace trace_processor
}  // namespace perfetto
