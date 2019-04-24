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

StringPoolAccessor::StringPoolAccessor(const std::deque<StringId>* deque,
                                       const StringPool* string_pool)
    : deque_(deque), string_pool_(string_pool) {}
StringPoolAccessor::~StringPoolAccessor() = default;

TsEndAccessor::TsEndAccessor(const std::deque<int64_t>* ts,
                             const std::deque<int64_t>* dur)
    : ts_(ts), dur_(dur) {}
TsEndAccessor::~TsEndAccessor() = default;

RowIdAccessor::RowIdAccessor(TableId table_id) : table_id_(table_id) {}
RowIdAccessor::~RowIdAccessor() = default;

RowAccessor::RowAccessor() = default;
RowAccessor::~RowAccessor() = default;

RefColumn::RefColumn(std::string col_name,
                     const std::deque<int64_t>* refs,
                     const std::deque<RefType>* types,
                     const TraceStorage* storage)
    : StorageColumn(col_name, false /* hidden */),
      refs_(refs),
      types_(types),
      storage_(storage) {}

void RefColumn::ReportResult(sqlite3_context* ctx, uint32_t row) const {
  auto ref = (*refs_)[row];
  auto type = (*types_)[row];
  if (type == RefType::kRefUtidLookupUpid) {
    auto upid = storage_->GetThread(static_cast<uint32_t>(ref)).upid;
    if (upid.has_value()) {
      sqlite_utils::ReportSqliteResult(ctx, upid.value());
    } else {
      sqlite3_result_null(ctx);
    }
  } else {
    sqlite_utils::ReportSqliteResult(ctx, ref);
  }
}

RefColumn::Bounds RefColumn::BoundFilter(int, sqlite3_value*) const {
  return Bounds{};
}

void RefColumn::Filter(int op,
                       sqlite3_value* value,
                       FilteredRowIndex* index) const {
  bool op_is_null = sqlite_utils::IsOpIsNull(op);
  auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
  index->FilterRows(
      [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
        auto ref = (*refs_)[row];
        auto type = (*types_)[row];
        if (type == RefType::kRefUtidLookupUpid) {
          auto upid = storage_->GetThread(static_cast<uint32_t>(ref)).upid;
          // Trying to filter null with any operation we currently handle
          // should return false.
          return upid.has_value() ? predicate(upid.value()) : op_is_null;
        }
        return predicate(ref);
      });
}

RefColumn::Comparator RefColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) { return -CompareRefsAsc(f, s); };
  }
  return [this](uint32_t f, uint32_t s) { return CompareRefsAsc(f, s); };
}

int RefColumn::CompareRefsAsc(uint32_t f, uint32_t s) const {
  auto ref_f = (*refs_)[f];
  auto ref_s = (*refs_)[s];

  auto type_f = (*types_)[f];
  auto type_s = (*types_)[s];

  base::Optional<int64_t> val_f = ref_f;
  base::Optional<int64_t> val_s = ref_s;
  if (type_f == RefType::kRefUtidLookupUpid) {
    val_f = storage_->GetThread(static_cast<uint32_t>(ref_f)).upid;
  }
  if (type_s == RefType::kRefUtidLookupUpid) {
    val_s = storage_->GetThread(static_cast<uint32_t>(ref_s)).upid;
  }

  bool has_f = val_f.has_value();
  bool has_s = val_s.has_value();
  if (has_f && has_s) {
    return sqlite_utils::CompareValuesAsc(val_f.value(), val_s.value());
  } else if (has_f && !has_s) {
    return 1;
  } else if (!has_f && has_s) {
    return -1;
  } else {
    return 0;
  }
}

}  // namespace trace_processor
}  // namespace perfetto
