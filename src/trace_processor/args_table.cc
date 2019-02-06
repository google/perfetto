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

#include "src/trace_processor/args_table.h"

#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

ArgsTable::ArgsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void ArgsTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<ArgsTable>(db, storage, "args");
}

StorageSchema ArgsTable::CreateStorageSchema() {
  const auto& args = storage_->args();
  return StorageSchema::Builder()
      .AddNumericColumn("arg_set_id", &args.set_ids())
      .AddStringColumn("flat_key", &args.flat_keys(), &storage_->string_pool())
      .AddStringColumn("key", &args.keys(), &storage_->string_pool())
      .AddColumn<ValueColumn>("int_value", VariadicType::kInt, storage_)
      .AddColumn<ValueColumn>("string_value", VariadicType::kString, storage_)
      .AddColumn<ValueColumn>("real_value", VariadicType::kReal, storage_)
      .Build({"arg_set_id", "key"});
}

uint32_t ArgsTable::RowCount() {
  return static_cast<uint32_t>(storage_->args().args_count());
}

int ArgsTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  // In the case of an id equality filter, we can do a very efficient lookup.
  if (qc.constraints().size() == 1) {
    auto id = static_cast<int>(schema().ColumnIndexFromName("arg_set_id"));
    const auto& cs = qc.constraints().back();
    if (cs.iColumn == id && sqlite_utils::IsOpEq(cs.op)) {
      info->estimated_cost = 1;
      return SQLITE_OK;
    }
  }

  // Otherwise, just give the worst case scenario.
  info->estimated_cost = static_cast<uint32_t>(storage_->args().args_count());
  return SQLITE_OK;
}

ArgsTable::ValueColumn::ValueColumn(std::string col_name,
                                    VariadicType type,
                                    const TraceStorage* storage)
    : StorageColumn(col_name, false /* hidden */),
      type_(type),
      storage_(storage) {}

void ArgsTable::ValueColumn::ReportResult(sqlite3_context* ctx,
                                          uint32_t row) const {
  const auto& value = storage_->args().arg_values()[row];
  if (value.type != type_) {
    sqlite3_result_null(ctx);
    return;
  }

  switch (type_) {
    case VariadicType::kInt:
      sqlite_utils::ReportSqliteResult(ctx, value.int_value);
      break;
    case VariadicType::kReal:
      sqlite_utils::ReportSqliteResult(ctx, value.real_value);
      break;
    case VariadicType::kString: {
      const char* str = storage_->GetString(value.string_value).c_str();
      sqlite3_result_text(ctx, str, -1, sqlite_utils::kSqliteStatic);
      break;
    }
  }
}

ArgsTable::ValueColumn::Bounds ArgsTable::ValueColumn::BoundFilter(
    int,
    sqlite3_value*) const {
  return Bounds{};
}

void ArgsTable::ValueColumn::Filter(int op,
                                    sqlite3_value* value,
                                    FilteredRowIndex* index) const {
  switch (type_) {
    case VariadicType::kInt: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
      index->FilterRows([this, &predicate, op_is_null](uint32_t row) {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_ ? predicate(arg.int_value) : op_is_null;
      });
      break;
    }
    case VariadicType::kReal: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<double>(op, value);
      index->FilterRows([this, &predicate, op_is_null](uint32_t row) {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_ ? predicate(arg.real_value) : op_is_null;
      });
      break;
    }
    case VariadicType::kString: {
      auto predicate = sqlite_utils::CreateStringPredicate(op, value);
      index->FilterRows([this, &predicate](uint32_t row) {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_
                   ? predicate(storage_->GetString(arg.string_value).c_str())
                   : predicate(nullptr);
      });
      break;
    }
  }
}

ArgsTable::ValueColumn::Comparator ArgsTable::ValueColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) { return -CompareRefsAsc(f, s); };
  }
  return [this](uint32_t f, uint32_t s) { return CompareRefsAsc(f, s); };
}

int ArgsTable::ValueColumn::CompareRefsAsc(uint32_t f, uint32_t s) const {
  const auto& arg_f = storage_->args().arg_values()[f];
  const auto& arg_s = storage_->args().arg_values()[s];

  if (arg_f.type == type_ && arg_s.type == type_) {
    switch (type_) {
      case VariadicType::kInt:
        return sqlite_utils::CompareValuesAsc(arg_f.int_value, arg_s.int_value);
      case VariadicType::kReal:
        return sqlite_utils::CompareValuesAsc(arg_f.real_value,
                                              arg_s.real_value);
      case VariadicType::kString: {
        const auto& f_str = storage_->GetString(arg_f.string_value);
        const auto& s_str = storage_->GetString(arg_s.string_value);
        return sqlite_utils::CompareValuesAsc(f_str, s_str);
      }
    }
  } else if (arg_s.type == type_) {
    return -1;
  } else if (arg_f.type == type_) {
    return 1;
  }
  return 0;
}

}  // namespace trace_processor
}  // namespace perfetto
