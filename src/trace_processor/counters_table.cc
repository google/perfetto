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

StorageSchema CountersTable::CreateStorageSchema() {
  const auto& cs = storage_->counters();
  return StorageSchema::Builder()
      .AddColumn<IdColumn>("id", TableId::kCounters)
      .AddOrderedNumericColumn("ts", &cs.timestamps())
      .AddStringColumn("name", &cs.name_ids(), &storage_->string_pool())
      .AddNumericColumn("value", &cs.values())
      .AddNumericColumn("dur", &cs.durations())
      .AddColumn<TsEndColumn>("ts_end", &cs.timestamps(), &cs.durations())
      .AddColumn<RefColumn>("ref", storage_)
      .AddStringColumn("ref_type", &cs.types(), &ref_types_)
      .Build({"name", "ts", "ref"});
}

std::unique_ptr<Table::Cursor> CountersTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  uint32_t count = static_cast<uint32_t>(storage_->counters().counter_count());
  auto it = CreateBestRowIteratorForGenericSchema(count, qc, argv);
  return std::unique_ptr<Table::Cursor>(
      new Cursor(std::move(it), schema_.mutable_columns()));
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
    : StorageColumn(col_name, false /* hidden */), storage_(storage) {}

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

void CountersTable::RefColumn::Filter(int op,
                                      sqlite3_value* value,
                                      FilteredRowIndex* index) const {
  auto predicate = sqlite_utils::CreatePredicate<int64_t>(op, value);
  index->FilterRows([this, &predicate](uint32_t row) {
    auto ref = storage_->counters().refs()[row];
    auto type = storage_->counters().types()[row];
    if (type == RefType::kRefUtidLookupUpid) {
      auto upid = storage_->GetThread(static_cast<uint32_t>(ref)).upid;
      // Trying to filter null with any operation we currently handle
      // should return false.
      return predicate(upid);
    }
    return predicate(ref);
  });
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
  } else if (type_s == RefType::kRefUtidLookupUpid) {
    auto upid_s = storage_->GetThread(static_cast<uint32_t>(ref_s)).upid;
    if (!upid_s.has_value())
      return 1;
  }
  return sqlite_utils::CompareValuesAsc(ref_f, ref_s);
}

}  // namespace trace_processor
}  // namespace perfetto
