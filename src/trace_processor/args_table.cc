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

#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {
PERFETTO_ALWAYS_INLINE
bool TreatedAsInteger(Variadic v) {
  return v.type == Variadic::Type::kInt || v.type == Variadic::Type::kBool ||
         v.type == Variadic::Type::kPointer || v.type == Variadic::Type::kUint;
}

PERFETTO_ALWAYS_INLINE
bool TreatedAsString(Variadic v) {
  return v.type == Variadic::Type::kString || v.type == Variadic::Type::kJson;
}

PERFETTO_ALWAYS_INLINE
int64_t AsInt64(Variadic v) {
  if (v.type == Variadic::Type::kInt)
    return v.int_value;
  if (v.type == Variadic::Type::kBool)
    return static_cast<int64_t>(v.bool_value);
  if (v.type == Variadic::Type::kUint)
    return static_cast<int64_t>(v.uint_value);
  if (v.type == Variadic::Type::kPointer)
    return static_cast<int64_t>(v.pointer_value);
  PERFETTO_FATAL("invalid Variadic type");
}

PERFETTO_ALWAYS_INLINE
StringId AsStringId(Variadic v) {
  if (v.type == Variadic::Type::kString)
    return v.string_value;
  if (v.type == Variadic::Type::kJson)
    return v.json_value;
  PERFETTO_FATAL("invalid Variadic type");
}
}  // namespace

ArgsTable::ArgsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void ArgsTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  SqliteTable::Register<ArgsTable>(db, storage, "args");
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
      storage_(storage) {
  PERFETTO_CHECK(type == Variadic::Type::kInt ||
                 type == Variadic::Type::kReal ||
                 type == Variadic::Type::kString);
}

void ArgsTable::ValueColumn::ReportResult(sqlite3_context* ctx,
                                          uint32_t row) const {
  const auto& value = storage_->args().arg_values()[row];
  switch (type_) {
    // Integer column, returns all integer-like variadic values (as an int64_t).
    case Variadic::Type::kInt: {
      if (!TreatedAsInteger(value)) {
        sqlite3_result_null(ctx);
        return;
      }
      sqlite_utils::ReportSqliteResult(ctx, AsInt64(value));
      return;
    }

      // Float column, returns only float values.
    case Variadic::Type::kReal: {
      if (value.type != Variadic::Type::kReal) {
        sqlite3_result_null(ctx);
        return;
      }
      sqlite_utils::ReportSqliteResult(ctx, value.real_value);
      return;
    }

      // String column, returns string & json variadic values (as a string).
    case Variadic::Type::kString: {
      if (!TreatedAsString(value)) {
        sqlite3_result_null(ctx);
        return;
      }
      const char* str = storage_->GetString(AsStringId(value)).c_str();
      sqlite3_result_text(ctx, str, -1, sqlite_utils::kSqliteStatic);
      return;
    }

    case Variadic::Type::kBool:
    case Variadic::Type::kUint:
    case Variadic::Type::kPointer:
    case Variadic::Type::kJson:
      PERFETTO_FATAL("Unexpected column type");
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
    // Integer column, returns all integer-like variadic values (as an int64_t).
    case Variadic::Type::kInt: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const Variadic& arg = storage_->args().arg_values()[row];
            if (!TreatedAsInteger(arg)) {
              return op_is_null;
            }
            return predicate(AsInt64(arg));
          });
      break;
    }

    // Float column, returns only float values.
    case Variadic::Type::kReal: {
      bool op_is_null = sqlite_utils::IsOpIsNull(op);
      auto predicate = sqlite_utils::CreateNumericPredicate<double>(op, value);
      index->FilterRows(
          [this, predicate, op_is_null](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            return arg.type == Variadic::Type::kReal ? predicate(arg.real_value)
                                                     : op_is_null;
          });
      break;
    }

    // String column, returns string & json variadic values (as a string).
    case Variadic::Type::kString: {
      auto predicate = sqlite_utils::CreateStringPredicate(op, value);
      index->FilterRows(
          [this, &predicate](uint32_t row) PERFETTO_ALWAYS_INLINE {
            const auto& arg = storage_->args().arg_values()[row];
            if (!TreatedAsString(arg)) {
              return predicate(nullptr);
            }
            return predicate(storage_->GetString(AsStringId(arg)).c_str());
          });
      break;
    }
    case Variadic::Type::kBool:
    case Variadic::Type::kUint:
    case Variadic::Type::kPointer:
    case Variadic::Type::kJson:
      PERFETTO_FATAL("Unexpected column type");
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
  switch (type_) {
    // Integer column, returns all integer-like variadic values (as an int64_t).
    case Variadic::Type::kInt: {
      if (TreatedAsInteger(arg_f) && TreatedAsInteger(arg_s)) {
        return sqlite_utils::CompareValuesAsc(AsInt64(arg_f), AsInt64(arg_s));
      } else if (TreatedAsInteger(arg_f)) {
        return 1;  // second value treated as null
      } else if (TreatedAsInteger(arg_s)) {
        return -1;  // first value treated as null
      }
      return 0;
    }

    // Float column, returns only float values.
    case Variadic::Type::kReal: {
      if (arg_f.type == Variadic::Type::kReal &&
          arg_s.type == Variadic::Type::kReal) {
        return sqlite_utils::CompareValuesAsc(arg_f.real_value,
                                              arg_s.real_value);
      } else if (arg_f.type == Variadic::Type::kReal) {
        return 1;  // second value treated as null
      } else if (arg_s.type == Variadic::Type::kReal) {
        return -1;  // first value treated as null
      }
      return 0;
    }

    // String column, returns string & json variadic values (as a string).
    case Variadic::Type::kString: {
      if (TreatedAsString(arg_f) && TreatedAsString(arg_s)) {
        const auto& f_str = storage_->GetString(AsStringId(arg_f));
        const auto& s_str = storage_->GetString(AsStringId(arg_s));
        return sqlite_utils::CompareValuesAsc(f_str, s_str);
      } else if (TreatedAsString(arg_f)) {
        return 1;  // second value treated as null
      } else if (TreatedAsString(arg_s)) {
        return -1;  // first value treated as null
      }
      return 0;
    }
    case Variadic::Type::kBool:
    case Variadic::Type::kUint:
    case Variadic::Type::kPointer:
    case Variadic::Type::kJson:
      PERFETTO_FATAL("Unexpected column type");
  }
  PERFETTO_FATAL("Never reached");  // for gcc
}

}  // namespace trace_processor
}  // namespace perfetto
