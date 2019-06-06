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
      .AddOrderedNumericColumn("arg_set_id", &args.set_ids())
      .AddStringColumn("flat_key", &args.flat_keys(), &storage_->string_pool())
      .AddStringColumn("key", &args.keys(), &storage_->string_pool())
      .AddColumn<ValueColumn>("int_value", Variadic::Type::kInt, storage_)
      .AddColumn<ValueColumn>("string_value", Variadic::Type::kString, storage_)
      .AddColumn<ValueColumn>("real_value", Variadic::Type::kReal, storage_)
      .Build({"arg_set_id", "key"});
}

uint32_t ArgsTable::RowCount() {
  return static_cast<uint32_t>(storage_->args().args_count());
}

int ArgsTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  if (HasEqConstraint(qc, "arg_set_id")) {
    info->estimated_cost = 1;
  } else {
    info->estimated_cost = static_cast<uint32_t>(storage_->args().args_count());
  }
  return SQLITE_OK;
}

ArgsTable::ValueColumn::ValueColumn(std::string col_name,
                                    Variadic::Type type,
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
    case Variadic::Type::kInt:
      sqlite_utils::ReportSqliteResult(ctx, value.int_value);
      break;
    case Variadic::Type::kUint:
      // BEWARE: uint64 is handled as signed int64 for SQLite operations.
      sqlite_utils::ReportSqliteResult(ctx,
                                       static_cast<int64_t>(value.uint_value));
      break;
    case Variadic::Type::kString: {
      const char* str = storage_->GetString(value.string_value).c_str();
      sqlite3_result_text(ctx, str, -1, sqlite_utils::kSqliteStatic);
      break;
    }
    case Variadic::Type::kReal:
      sqlite_utils::ReportSqliteResult(ctx, value.real_value);
      break;
    case Variadic::Type::kPointer:
      // BEWARE: pointers are handled as signed int64 for SQLite operations.
      sqlite_utils::ReportSqliteResult(
          ctx, static_cast<int64_t>(value.pointer_value));
      break;
    case Variadic::Type::kBool:
      sqlite_utils::ReportSqliteResult(ctx, value.bool_value);
      break;
    case Variadic::Type::kJson:
      sqlite_utils::ReportSqliteResult(ctx, value.json_value);
      break;
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
    case Variadic::Type::kInt: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            return arg.type == type_ ? predicate(arg.int_value) : op_is_null;
          });
      break;
    }
    case Variadic::Type::kUint: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      // BEWARE: uint64 is handled as signed int64 for SQLite operations.
      auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            return arg.type == type_
                       ? predicate(static_cast<int64_t>(arg.uint_value))
                       : op_is_null;
          });
      break;
    }
    case Variadic::Type::kString: {
      auto predicate = sqlite_utils::CreateStringPredicate(op, value);
      index->FilterRows([this,
                         &predicate](uint32_t row) PERFETTO_ALWAYS_INLINE {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_
                   ? predicate(storage_->GetString(arg.string_value).c_str())
                   : predicate(nullptr);
      });
      break;
    }
    case Variadic::Type::kReal: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<double>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            return arg.type == type_ ? predicate(arg.real_value) : op_is_null;
          });
      break;
    }
    case Variadic::Type::kPointer: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      // BEWARE: pointers are handled as signed int64 for SQLite operations.
      auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            return arg.type == type_
                       ? predicate(static_cast<int64_t>(arg.pointer_value))
                       : op_is_null;
          });
      break;
    }
    case Variadic::Type::kBool: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<bool>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            return arg.type == type_ ? predicate(arg.bool_value) : op_is_null;
          });
      break;
    }
    case Variadic::Type::kJson: {
      auto predicate = sqlite_utils::CreateStringPredicate(op, value);
      index->FilterRows([this,
                         &predicate](uint32_t row) PERFETTO_ALWAYS_INLINE {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_
                   ? predicate(storage_->GetString(arg.json_value).c_str())
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
      case Variadic::Type::kInt:
        return sqlite_utils::CompareValuesAsc(arg_f.int_value, arg_s.int_value);
      case Variadic::Type::kUint:
        // BEWARE: uint64 is handled as signed int64 for SQLite operations.
        return sqlite_utils::CompareValuesAsc(
            static_cast<int64_t>(arg_f.uint_value),
            static_cast<int64_t>(arg_s.uint_value));
      case Variadic::Type::kString: {
        const auto& f_str = storage_->GetString(arg_f.string_value);
        const auto& s_str = storage_->GetString(arg_s.string_value);
        return sqlite_utils::CompareValuesAsc(f_str, s_str);
      }
      case Variadic::Type::kReal:
        return sqlite_utils::CompareValuesAsc(arg_f.real_value,
                                              arg_s.real_value);
      case Variadic::Type::kPointer:
        // BEWARE: pointers are handled as signed int64 for SQLite operations.
        return sqlite_utils::CompareValuesAsc(
            static_cast<int64_t>(arg_f.pointer_value),
            static_cast<int64_t>(arg_s.pointer_value));
      case Variadic::Type::kBool:
        return sqlite_utils::CompareValuesAsc(arg_f.bool_value,
                                              arg_s.bool_value);
      case Variadic::Type::kJson: {
        const auto& f_str = storage_->GetString(arg_f.json_value);
        const auto& s_str = storage_->GetString(arg_s.json_value);
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
