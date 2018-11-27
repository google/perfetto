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

#include "src/trace_processor/counters_table.h"

#include "src/trace_processor/storage_cursor.h"
#include "src/trace_processor/table_utils.h"

namespace perfetto {
namespace trace_processor {

CountersTable::CountersTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {
  ref_types_.resize(RefType::kRefMax);
  ref_types_[RefType::kRefNoRef] = "";
  ref_types_[RefType::kRefUtid] = "utid";
  ref_types_[RefType::kRefCpuId] = "cpu";
  ref_types_[RefType::kRefIrq] = "irq";
  ref_types_[RefType::kRefSoftIrq] = "softirq";
  ref_types_[RefType::kRefUpid] = "upid";
  ref_types_[RefType::kRefUtidLookupUpid] = "upid";
}

void CountersTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<CountersTable>(db, storage, "counters");
}

Table::Schema CountersTable::CreateSchema(int, const char* const*) {
  const auto& counters = storage_->counters();

  std::unique_ptr<StorageSchema::Column> cols[] = {
      StorageSchema::NumericColumnPtr("ts", &counters.timestamps(),
                                      false /* hidden */, true /* ordered */),
      StorageSchema::StringColumnPtr("name", &counters.name_ids(),
                                     &storage_->string_pool()),
      StorageSchema::NumericColumnPtr("value", &counters.values()),
      StorageSchema::NumericColumnPtr("dur", &counters.durations()),
      StorageSchema::TsEndPtr("ts_end", &counters.timestamps(),
                              &counters.durations()),
      std::unique_ptr<RefColumn>(new RefColumn("ref", storage_)),
      StorageSchema::StringColumnPtr("ref_type", &counters.types(),
                                     &ref_types_)};
  schema_ = StorageSchema({
      std::make_move_iterator(std::begin(cols)),
      std::make_move_iterator(std::end(cols)),
  });
  return schema_.ToTableSchema({"name", "ts", "ref"});
}

std::unique_ptr<Table::Cursor> CountersTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  uint32_t count = static_cast<uint32_t>(storage_->counters().counter_count());
  auto it = table_utils::CreateBestRowIteratorForGenericSchema(schema_, count,
                                                               qc, argv);
  return std::unique_ptr<Table::Cursor>(
      new StorageCursor(std::move(it), schema_.ToColumnReporters()));
}

int CountersTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost =
      static_cast<uint32_t>(storage_->counters().counter_count());

  // Only the string columns are handled by SQLite
  info->order_by_consumed = true;
  size_t name_index = schema_.ColumnIndexFromName("name");
  size_t ref_type_index = schema_.ColumnIndexFromName("ref_type");
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    info->omit[i] =
        qc.constraints()[i].iColumn != static_cast<int>(name_index) &&
        qc.constraints()[i].iColumn != static_cast<int>(ref_type_index);
  }

  return SQLITE_OK;
}

CountersTable::RefColumn::RefColumn(std::string col_name,
                                    const TraceStorage* storage)
    : Column(col_name, false), storage_(storage) {}

void CountersTable::RefColumn::ReportResult(sqlite3_context* ctx,
                                            uint32_t row) const {
  auto ref = storage_->counters().refs()[row];
  auto type = storage_->counters().types()[row];
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

CountersTable::RefColumn::Bounds CountersTable::RefColumn::BoundFilter(
    int,
    sqlite3_value*) const {
  return Bounds{};
}

CountersTable::RefColumn::Predicate CountersTable::RefColumn::Filter(
    int op,
    sqlite3_value* value) const {
  auto binary_op = sqlite_utils::GetPredicateForOp<int64_t>(op);
  int64_t extracted = sqlite_utils::ExtractSqliteValue<int64_t>(value);
  return [this, binary_op, extracted](uint32_t idx) {
    auto ref = storage_->counters().refs()[idx];
    auto type = storage_->counters().types()[idx];
    if (type == RefType::kRefUtidLookupUpid) {
      auto upid = storage_->GetThread(static_cast<uint32_t>(ref)).upid;
      // Trying to filter null with any operation we currently handle
      // should return false.
      return upid.has_value() && binary_op(upid.value(), extracted);
    }
    return binary_op(ref, extracted);
  };
}

CountersTable::RefColumn::Comparator CountersTable::RefColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) { return -CompareRefsAsc(f, s); };
  }
  return [this](uint32_t f, uint32_t s) { return CompareRefsAsc(f, s); };
}

int CountersTable::RefColumn::CompareRefsAsc(uint32_t f, uint32_t s) const {
  auto ref_f = storage_->counters().refs()[f];
  auto ref_s = storage_->counters().refs()[s];

  auto type_f = storage_->counters().types()[f];
  auto type_s = storage_->counters().types()[s];

  if (type_f == RefType::kRefUtidLookupUpid) {
    auto upid_f = storage_->GetThread(static_cast<uint32_t>(ref_f)).upid;
    if (type_s == RefType::kRefUtidLookupUpid) {
      auto upid_s = storage_->GetThread(static_cast<uint32_t>(ref_s)).upid;
      if (!upid_f.has_value() && !upid_s.has_value()) {
        return 0;
      } else if (!upid_f.has_value()) {
        return -1;
      } else if (!upid_s.has_value()) {
        return 1;
      }
      return sqlite_utils::CompareValuesAsc(upid_f.value(), upid_s.value());
    }
    if (!upid_f.has_value())
      return -1;
  }
  return sqlite_utils::CompareValuesAsc(ref_f, ref_s);
}

}  // namespace trace_processor
}  // namespace perfetto
