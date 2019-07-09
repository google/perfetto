/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/metadata_table.h"
#include "src/trace_processor/sqlite_utils.h"
#include "src/trace_processor/storage_columns.h"
#include "src/trace_processor/storage_schema.h"

namespace perfetto {
namespace trace_processor {

MetadataTable::MetadataTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void MetadataTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<MetadataTable>(db, storage, "metadata");
}

StorageSchema MetadataTable::CreateStorageSchema() {
  return StorageSchema::Builder()
      .AddColumn<StringColumn<MetadataKeyNameAccessor>>(
          "name", &storage_->metadata().keys())
      .AddColumn<StringColumn<MetadataKeyTypeAccessor>>(
          "key_type", &storage_->metadata().keys())
      .AddColumn<ValueColumn>("int_value", Variadic::Type::kInt, storage_)
      .AddColumn<ValueColumn>("str_value", Variadic::Type::kString, storage_)
      .Build({"name"});
}

uint32_t MetadataTable::RowCount() {
  return static_cast<uint32_t>(storage_->metadata().keys().size());
}

int MetadataTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  return SQLITE_OK;
}

MetadataTable::MetadataKeyNameAccessor::MetadataKeyNameAccessor(
    const std::deque<metadata::KeyIDs>* keys)
    : keys_(keys) {}

MetadataTable::MetadataKeyNameAccessor::~MetadataKeyNameAccessor() = default;

MetadataTable::MetadataKeyTypeAccessor::MetadataKeyTypeAccessor(
    const std::deque<metadata::KeyIDs>* keys)
    : keys_(keys) {}

MetadataTable::MetadataKeyTypeAccessor::~MetadataKeyTypeAccessor() = default;

MetadataTable::ValueColumn::ValueColumn(std::string col_name,
                                        Variadic::Type type,
                                        const TraceStorage* storage)
    : StorageColumn(col_name, false /* hidden */),
      type_(type),
      storage_(storage) {
  PERFETTO_CHECK(type == Variadic::Type::kInt ||
                 type == Variadic::Type::kString);
}

void MetadataTable::ValueColumn::ReportResult(sqlite3_context* ctx,
                                              uint32_t row) const {
  const auto& metadata = storage_->metadata();
  auto value_type = metadata::kValueTypes[metadata.keys()[row]];
  if (value_type != type_) {
    sqlite3_result_null(ctx);
    return;
  }

  if (value_type == Variadic::Type::kInt) {
    sqlite_utils::ReportSqliteResult(ctx, metadata.values()[row].int_value);
    return;
  }
  if (value_type == Variadic::Type::kString) {
    const char* str =
        storage_->GetString(metadata.values()[row].string_value).c_str();
    sqlite3_result_text(ctx, str, -1, sqlite_utils::kSqliteStatic);
    return;
  }
  PERFETTO_FATAL("Unimplemented metadata value type.");
}

MetadataTable::ValueColumn::Bounds MetadataTable::ValueColumn::BoundFilter(
    int,
    sqlite3_value*) const {
  return Bounds{};
}

void MetadataTable::ValueColumn::Filter(int op,
                                        sqlite3_value* value,
                                        FilteredRowIndex* index) const {
  if (type_ == Variadic::Type::kInt) {
    bool op_is_null = sqlite_utils::IsOpIsNull(op);
    auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
    index->FilterRows(
        [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
          const auto& arg = storage_->metadata().values()[row];
          return arg.type == type_ ? predicate(arg.int_value) : op_is_null;
        });
    return;
  }
  if (type_ == Variadic::Type::kString) {
    auto predicate = sqlite_utils::CreateStringPredicate(op, value);
    index->FilterRows([this, &predicate](uint32_t row) PERFETTO_ALWAYS_INLINE {
      const auto& arg = storage_->metadata().values()[row];
      return arg.type == type_
                 ? predicate(storage_->GetString(arg.string_value).c_str())
                 : predicate(nullptr);
    });
    return;
  }
  PERFETTO_FATAL("Unimplemented metadata value type.");
}

MetadataTable::ValueColumn::Comparator MetadataTable::ValueColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) { return -CompareRefsAsc(f, s); };
  }
  return [this](uint32_t f, uint32_t s) { return CompareRefsAsc(f, s); };
}

int MetadataTable::ValueColumn::CompareRefsAsc(uint32_t f, uint32_t s) const {
  const auto& arg_f = storage_->metadata().values()[f];
  const auto& arg_s = storage_->metadata().values()[s];

  if (arg_f.type == type_ && arg_s.type == type_) {
    if (type_ == Variadic::Type::kInt) {
      return sqlite_utils::CompareValuesAsc(arg_f.int_value, arg_s.int_value);
    }
    if (type_ == Variadic::Type::kString) {
      const auto& f_str = storage_->GetString(arg_f.string_value);
      const auto& s_str = storage_->GetString(arg_s.string_value);
      return sqlite_utils::CompareValuesAsc(f_str, s_str);
    }
    PERFETTO_FATAL("Unimplemented metadata value type.");
  } else if (arg_s.type == type_) {
    return -1;
  } else if (arg_f.type == type_) {
    return 1;
  }
  return 0;
}

}  // namespace trace_processor
}  // namespace perfetto
