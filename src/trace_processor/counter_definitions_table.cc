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

#include "src/trace_processor/counter_definitions_table.h"

namespace perfetto {
namespace trace_processor {

CounterDefinitionsTable::CounterDefinitionsTable(sqlite3*,
                                                 const TraceStorage* storage)
    : storage_(storage) {
  ref_types_.resize(RefType::kRefMax);
  ref_types_[RefType::kRefNoRef] = nullptr;
  ref_types_[RefType::kRefUtid] = "utid";
  ref_types_[RefType::kRefCpuId] = "cpu";
  ref_types_[RefType::kRefIrq] = "irq";
  ref_types_[RefType::kRefSoftIrq] = "softirq";
  ref_types_[RefType::kRefUpid] = "upid";
  ref_types_[RefType::kRefUtidLookupUpid] = "upid";
}

void CounterDefinitionsTable::RegisterTable(sqlite3* db,
                                            const TraceStorage* storage) {
  Table::Register<CounterDefinitionsTable>(db, storage, "counter_definitions");
}

StorageSchema CounterDefinitionsTable::CreateStorageSchema() {
  const auto& cs = storage_->counter_definitions();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("counter_id", RowAccessor())
      .AddStringColumn("name", &cs.name_ids(), &storage_->string_pool())
      .AddColumn<RefColumn>("ref", &cs.refs(), &cs.types(), storage_)
      .AddStringColumn("ref_type", &cs.types(), &ref_types_)
      .Build({"counter_id"});
}

uint32_t CounterDefinitionsTable::RowCount() {
  return storage_->counter_definitions().size();
}

int CounterDefinitionsTable::BestIndex(const QueryConstraints& qc,
                                       BestIndexInfo* info) {
  info->estimated_cost = EstimateCost(qc);

  // Only the string columns are handled by SQLite
  size_t name_index = schema().ColumnIndexFromName("name");
  size_t ref_type_index = schema().ColumnIndexFromName("ref_type");
  info->order_by_consumed = true;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    auto col = static_cast<size_t>(qc.constraints()[i].iColumn);
    info->omit[i] = col != name_index && col != ref_type_index;
  }

  return SQLITE_OK;
}

uint32_t CounterDefinitionsTable::EstimateCost(const QueryConstraints& qc) {
  // If there is a constraint on the counter id, we can efficiently filter
  // to a single row.
  if (HasEqConstraint(qc, "counter_id"))
    return 1;

  auto eq_name = HasEqConstraint(qc, "name");
  auto eq_ref = HasEqConstraint(qc, "ref");
  auto eq_ref_type = HasEqConstraint(qc, "ref_type");

  // If there is a constraint on all three columns, we are going to only return
  // exaclty one row for sure so make the cost 1.
  if (eq_name && eq_ref && eq_ref_type)
    return 1;
  else if (eq_name && eq_ref)
    return 10;
  else if (eq_name)
    return 100;
  return RowCount();
}

CounterDefinitionsTable::RefColumn::RefColumn(std::string col_name,
                                              const std::deque<int64_t>* refs,
                                              const std::deque<RefType>* types,
                                              const TraceStorage* storage)
    : StorageColumn(col_name, false /* hidden */),
      refs_(refs),
      types_(types),
      storage_(storage) {}

void CounterDefinitionsTable::RefColumn::ReportResult(sqlite3_context* ctx,
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

CounterDefinitionsTable::RefColumn::Bounds
CounterDefinitionsTable::RefColumn::BoundFilter(int, sqlite3_value*) const {
  return Bounds{};
}

void CounterDefinitionsTable::RefColumn::Filter(int op,
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

CounterDefinitionsTable::RefColumn::Comparator
CounterDefinitionsTable::RefColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) { return -CompareRefsAsc(f, s); };
  }
  return [this](uint32_t f, uint32_t s) { return CompareRefsAsc(f, s); };
}

int CounterDefinitionsTable::RefColumn::CompareRefsAsc(uint32_t f,
                                                       uint32_t s) const {
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
