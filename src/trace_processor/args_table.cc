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

base::Optional<Table::Schema> ArgsTable::Init(int, const char* const*) {
  const auto& args = storage_->args();
  std::unique_ptr<StorageColumn> cols[] = {
      std::unique_ptr<IdColumn>(new IdColumn("id", storage_, &args.ids())),
      StringColumnPtr("flat_key", &args.flat_keys(), &storage_->string_pool()),
      StringColumnPtr("key", &args.keys(), &storage_->string_pool()),
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
  auto it = CreateBestRowIteratorForGenericSchema(count, qc, argv);
  return std::unique_ptr<Cursor>(
      new Cursor(std::move(it), schema_.mutable_columns()));
}

int ArgsTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  // In the case of an id equality filter, we can do a very efficient lookup.
  if (qc.constraints().size() == 1) {
    auto id = static_cast<int>(schema_.ColumnIndexFromName("id"));
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

ArgsTable::IdColumn::IdColumn(std::string col_name,
                              const TraceStorage* storage,
                              const std::deque<RowId>* ids)
    : NumericColumn(col_name, ids, false, false), storage_(storage) {}

void ArgsTable::IdColumn::Filter(int op,
                                 sqlite3_value* value,
                                 FilteredRowIndex* index) const {
  if (!sqlite_utils::IsOpEq(op)) {
    NumericColumn::Filter(op, value, index);
    return;
  }
  auto id = sqlite_utils::ExtractSqliteValue<RowId>(value);
  const auto& args_for_id = storage_->args().args_for_id();
  auto it_pair = args_for_id.equal_range(id);

  auto size = static_cast<size_t>(std::distance(it_pair.first, it_pair.second));
  std::vector<uint32_t> rows(size);
  size_t i = 0;
  for (auto it = it_pair.first; it != it_pair.second; it++) {
    rows[i++] = it->second;
  }
  index->IntersectRows(std::move(rows));
}

ArgsTable::ValueColumn::ValueColumn(std::string col_name,
                                    VarardicType type,
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
    case VarardicType::kInt:
      sqlite_utils::ReportSqliteResult(ctx, value.int_value);
      break;
    case VarardicType::kReal:
      sqlite_utils::ReportSqliteResult(ctx, value.real_value);
      break;
    case VarardicType::kString: {
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
    case VarardicType::kInt: {
      auto predicate = sqlite_utils::CreatePredicate<int64_t>(op, value);
      index->FilterRows([this, &predicate](uint32_t row) {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_ ? predicate(arg.int_value)
                                 : predicate(base::nullopt);
      });
      break;
    }
    case VarardicType::kReal: {
      auto predicate = sqlite_utils::CreatePredicate<double>(op, value);
      index->FilterRows([this, &predicate](uint32_t row) {
        const auto& arg = storage_->args().arg_values()[row];
        return arg.type == type_ ? predicate(arg.real_value)
                                 : predicate(base::nullopt);
      });
      break;
    }
    case VarardicType::kString: {
      auto predicate = sqlite_utils::CreatePredicate<std::string>(op, value);
      index->FilterRows([this, &predicate](uint32_t row) {
        const auto& arg = storage_->args().arg_values()[row];
        const auto& str = storage_->GetString(arg.string_value);
        return arg.type == type_ ? predicate(str) : predicate(base::nullopt);
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
