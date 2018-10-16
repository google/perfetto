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

#include "perfetto/base/logging.h"
#include "src/trace_processor/query_constraints.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

PERFETTO_ALWAYS_INLINE int CompareCountersOnColumn(
    const TraceStorage* storage,
    size_t f_idx,
    size_t s_idx,
    const QueryConstraints::OrderBy& ob) {
  const auto& co = storage->counters();
  switch (ob.iColumn) {
    case CountersTable::Column::kTimestamp:
      return CompareValues(co.timestamps(), f_idx, s_idx, ob.desc);
    case CountersTable::Column::kValue:
      return CompareValues(co.values(), f_idx, s_idx, ob.desc);
    case CountersTable::Column::kName:
      return CompareValues(co.name_ids(), f_idx, s_idx, ob.desc);
    case CountersTable::Column::kRef:
      return CompareValues(co.refs(), f_idx, s_idx, ob.desc);
    case CountersTable::Column::kDuration:
      return CompareValues(co.durations(), f_idx, s_idx, ob.desc);
    case CountersTable::Column::kValueDelta:
      return CompareValues(co.value_deltas(), f_idx, s_idx, ob.desc);
    case CountersTable::Column::kRefType:
      return CompareValues(co.types(), f_idx, s_idx, ob.desc);
    default:
      PERFETTO_FATAL("Unexpected column %d", ob.iColumn);
  }
}

PERFETTO_ALWAYS_INLINE int CompareCounters(
    const TraceStorage* storage,
    size_t f_idx,
    size_t s_idx,
    const std::vector<QueryConstraints::OrderBy>& order_by) {
  for (const auto& ob : order_by) {
    int c = CompareCountersOnColumn(storage, f_idx, s_idx, ob);
    if (c != 0)
      return c;
  }
  return 0;
}

}  // namespace

CountersTable::CountersTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void CountersTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<CountersTable>(db, storage, "counters");
}

Table::Schema CountersTable::CreateSchema(int, const char* const*) {
  return Schema(
      {
          Table::Column(Column::kTimestamp, "ts", ColumnType::kUlong),
          Table::Column(Column::kName, "name", ColumnType::kString),
          Table::Column(Column::kValue, "value", ColumnType::kUlong),
          Table::Column(Column::kDuration, "dur", ColumnType::kUlong),
          Table::Column(Column::kValueDelta, "value_delta", ColumnType::kUlong),
          Table::Column(Column::kRef, "ref", ColumnType::kUint),
          Table::Column(Column::kRefType, "ref_type", ColumnType::kString),
      },
      {Column::kName, Column::kTimestamp, Column::kRef});
}

std::unique_ptr<Table::Cursor> CountersTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_, qc, argv));
}

int CountersTable::BestIndex(const QueryConstraints&, BestIndexInfo* info) {
  // TODO(taylori): Work out cost dependant on constraints.
  info->estimated_cost =
      static_cast<uint32_t>(storage_->counters().counter_count());
  info->order_by_consumed = true;

  return SQLITE_OK;
}

CountersTable::Cursor::Cursor(const TraceStorage* storage,
                              const QueryConstraints& qc,
                              sqlite3_value** argv)
    : storage_(storage) {
  const auto& counters = storage->counters();

  std::vector<bool> filter(counters.counter_count(), true);
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& cs = qc.constraints()[i];
    auto* v = argv[i];
    switch (cs.iColumn) {
      case CountersTable::Column::kTimestamp:
        FilterColumn(counters.timestamps(), 0, cs, v, &filter);
        break;
      case CountersTable::Column::kValue:
        FilterColumn(counters.values(), 0, cs, v, &filter);
        break;
      case CountersTable::Column::kName:
        FilterColumn(counters.name_ids(), 0, cs, v, &filter);
        break;
      case CountersTable::Column::kRef:
        FilterColumn(counters.refs(), 0, cs, v, &filter);
        break;
      case CountersTable::Column::kDuration:
        FilterColumn(counters.durations(), 0, cs, v, &filter);
        break;
      case CountersTable::Column::kValueDelta:
        FilterColumn(counters.value_deltas(), 0, cs, v, &filter);
        break;
      case CountersTable::Column::kRefType: {
        // TODO(lalitm): add support for filtering here.
      }
    }
  }

  sorted_rows_ = CreateSortedIndexFromFilter(
      0, filter, [this, &qc](uint32_t f, uint32_t s) {
        return CompareCounters(storage_, f, s, qc.order_by()) < 0;
      });
}

int CountersTable::Cursor::Column(sqlite3_context* context, int N) {
  size_t row = sorted_rows_[next_row_idx_];
  switch (N) {
    case Column::kTimestamp: {
      sqlite3_result_int64(
          context,
          static_cast<int64_t>(storage_->counters().timestamps()[row]));
      break;
    }
    case Column::kValue: {
      sqlite3_result_int64(
          context, static_cast<int64_t>(storage_->counters().values()[row]));
      break;
    }
    case Column::kName: {
      sqlite3_result_text(
          context,
          storage_->GetString(storage_->counters().name_ids()[row]).c_str(), -1,
          nullptr);
      break;
    }
    case Column::kRef: {
      sqlite3_result_int64(
          context, static_cast<int64_t>(storage_->counters().refs()[row]));
      break;
    }
    case Column::kRefType: {
      switch (storage_->counters().types()[row]) {
        case RefType::kCPU_ID: {
          sqlite3_result_text(context, "cpu", -1, nullptr);
          break;
        }
        case RefType::kUTID: {
          sqlite3_result_text(context, "utid", -1, nullptr);
          break;
        }
        case RefType::kNoRef: {
          sqlite3_result_null(context);
          break;
        }
        case RefType::kIrq: {
          sqlite3_result_text(context, "irq", -1, nullptr);
          break;
        }
        case RefType::kSoftIrq: {
          sqlite3_result_text(context, "softirq", -1, nullptr);
          break;
        }
      }
      break;
    }
    case Column::kDuration: {
      sqlite3_result_int64(
          context, static_cast<int64_t>(storage_->counters().durations()[row]));
      break;
    }
    case Column::kValueDelta: {
      sqlite3_result_int64(
          context,
          static_cast<int64_t>(storage_->counters().value_deltas()[row]));
      break;
    }
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int CountersTable::Cursor::Next() {
  next_row_idx_++;
  return SQLITE_OK;
}

int CountersTable::Cursor::Eof() {
  return next_row_idx_ >= sorted_rows_.size();
}

}  // namespace trace_processor
}  // namespace perfetto
