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
      .AddColumn<RefColumn>("ref", &cs.refs(), &cs.types(), storage_)
      .AddStringColumn("ref_type", &cs.types(), &ref_types_)
      .AddNumericColumn("arg_set_id", &cs.arg_set_ids())
      .Build({"name", "ts", "ref"});
}

uint32_t CountersTable::RowCount() {
  return static_cast<uint32_t>(storage_->counters().counter_count());
}

int CountersTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost =
      static_cast<uint32_t>(storage_->counters().counter_count());

  // Only the string columns are handled by SQLite
  info->order_by_consumed = true;
  size_t name_index = schema().ColumnIndexFromName("name");
  size_t ref_type_index = schema().ColumnIndexFromName("ref_type");
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    info->omit[i] =
        qc.constraints()[i].iColumn != static_cast<int>(name_index) &&
        qc.constraints()[i].iColumn != static_cast<int>(ref_type_index);
  }

  return SQLITE_OK;
}

CountersTable::RefColumn::RefColumn(std::string col_name,
                                    const std::deque<int64_t>* refs,
                                    const std::deque<RefType>* types,
                                    const TraceStorage* storage)
    : StorageColumn(col_name, false /* hidden */),
      refs_(refs),
      types_(types),
      storage_(storage) {}

void CountersTable::RefColumn::ReportResult(sqlite3_context* ctx,
                                            uint32_t row) const {
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

CountersTable::RefColumn::Bounds CountersTable::RefColumn::BoundFilter(
    int,
    sqlite3_value*) const {
  return Bounds{};
}

void CountersTable::RefColumn::Filter(int op,
                                      sqlite3_value* value,
                                      FilteredRowIndex* index) const {
  bool op_is_null = sqlite_utils::IsOpIsNull(op);
  auto predicate = sqlite_utils::CreateNumericPredicate<int64_t>(op, value);
  index->FilterRows([this, &predicate, op_is_null](uint32_t row) {
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

CountersTable::RefColumn::Comparator CountersTable::RefColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) { return -CompareRefsAsc(f, s); };
  }
  return [this](uint32_t f, uint32_t s) { return CompareRefsAsc(f, s); };
}

int CountersTable::RefColumn::CompareRefsAsc(uint32_t f, uint32_t s) const {
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

  if (val_f.has_value() && val_s.has_value()) {
    return sqlite_utils::CompareValuesAsc(val_f.value(), val_s.value());
  } else if (!val_f.has_value()) {
    return val_s.has_value() ? -1 : 0;
  } else {
    return 1;
  }
}

}  // namespace trace_processor
}  // namespace perfetto
