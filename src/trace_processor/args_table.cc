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

#include "src/trace_processor/storage_cursor.h"
#include "src/trace_processor/table_utils.h"

namespace perfetto {
namespace trace_processor {

ArgsTable::ArgsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void ArgsTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<ArgsTable>(db, storage, "args");
}

Table::Schema ArgsTable::CreateSchema(int, const char* const*) {
  const auto& args = storage_->args();
  std::unique_ptr<StorageSchema::Column> cols[] = {
      StorageSchema::NumericColumnPtr("id", &args.ids()),
      StorageSchema::StringColumnPtr("flat_key", &args.flat_keys(),
                                     &storage_->string_pool()),
      StorageSchema::StringColumnPtr("key", &args.keys(),
                                     &storage_->string_pool()),
      std::unique_ptr<ValueColumn>(
          new ValueColumn("int_value", VarardicType::kInt, storage_)),
      std::unique_ptr<ValueColumn>(
          new ValueColumn("string_value", VarardicType::kString, storage_)),
      std::unique_ptr<ValueColumn>(
          new ValueColumn("real_value", VarardicType::kReal, storage_))};
  schema_ = StorageSchema({
      std::make_move_iterator(std::begin(cols)),
      std::make_move_iterator(std::end(cols)),
  });
  return schema_.ToTableSchema({"id", "key"});
}

std::unique_ptr<Table::Cursor> ArgsTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  uint32_t count = static_cast<uint32_t>(storage_->args().args_count());
  auto it = table_utils::CreateBestRowIteratorForGenericSchema(schema_, count,
                                                               qc, argv);
  return std::unique_ptr<Table::Cursor>(
      new StorageCursor(std::move(it), schema_.ToColumnReporters()));
}

int ArgsTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  // TODO(lalitm): implement BestIndex properly.
  return SQLITE_OK;
}

ArgsTable::ValueColumn::ValueColumn(std::string col_name,
                                    VarardicType type,
                                    const TraceStorage* storage)
    : Column(col_name, false), type_(type), storage_(storage) {}

void ArgsTable::ValueColumn::ReportResult(sqlite3_context* ctx,
                                          uint32_t row) const {
  const auto& value = storage_->args().arg_values()[row];
  if (value.type != type_) {
    sqlite3_result_null(ctx);
    return;
  }

  switch (type_) {
    case VarardicType::kInt:
      sqlite_utils::ReportSqliteResult(ctx, value.int_value);
      break;
    case VarardicType::kReal:
      sqlite_utils::ReportSqliteResult(ctx, value.real_value);
      break;
    case VarardicType::kString: {
      const auto kSqliteStatic = reinterpret_cast<sqlite3_destructor_type>(0);
      const char* str = storage_->GetString(value.string_value).c_str();
      sqlite3_result_text(ctx, str, -1, kSqliteStatic);
      break;
    }
  }
}

ArgsTable::ValueColumn::Bounds ArgsTable::ValueColumn::BoundFilter(
    int,
    sqlite3_value*) const {
  return Bounds{};
}

ArgsTable::ValueColumn::Predicate ArgsTable::ValueColumn::Filter(
    int op,
    sqlite3_value* value) const {
  switch (type_) {
    case VarardicType::kInt: {
      auto binary_op = sqlite_utils::GetPredicateForOp<int64_t>(op);
      int64_t extracted = sqlite_utils::ExtractSqliteValue<int64_t>(value);
      return [this, binary_op, extracted](uint32_t idx) {
        const auto& arg = storage_->args().arg_values()[idx];
        return arg.type == type_ && binary_op(arg.int_value, extracted);
      };
    }
    case VarardicType::kReal: {
      auto binary_op = sqlite_utils::GetPredicateForOp<double>(op);
      double extracted = sqlite_utils::ExtractSqliteValue<double>(value);
      return [this, binary_op, extracted](uint32_t idx) {
        const auto& arg = storage_->args().arg_values()[idx];
        return arg.type == type_ && binary_op(arg.real_value, extracted);
      };
    }
    case VarardicType::kString: {
      auto binary_op = sqlite_utils::GetPredicateForOp<std::string>(op);
      const auto* extracted =
          reinterpret_cast<const char*>(sqlite3_value_text(value));
      return [this, binary_op, extracted](uint32_t idx) {
        const auto& arg = storage_->args().arg_values()[idx];
        const char* str = storage_->GetString(arg.string_value).c_str();
        return arg.type == type_ && binary_op(str, extracted);
      };
    }
  }
  PERFETTO_CHECK(false);
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
      case VarardicType::kInt:
        return sqlite_utils::CompareValuesAsc(arg_f.int_value, arg_s.int_value);
      case VarardicType::kReal:
        return sqlite_utils::CompareValuesAsc(arg_f.real_value,
                                              arg_s.real_value);
      case VarardicType::kString: {
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
