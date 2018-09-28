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

#include "src/trace_processor/window_operator_table.h"

#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {
using namespace sqlite_utils;
}  // namespace

WindowOperatorTable::WindowOperatorTable(sqlite3*, const TraceStorage*) {}

void WindowOperatorTable::RegisterTable(sqlite3* db,
                                        const TraceStorage* storage) {
  Table::Register<WindowOperatorTable>(db, storage, "window", true);
}

Table::Schema WindowOperatorTable::CreateSchema(int, const char* const*) {
  const bool kHidden = true;
  return Schema(
      {
          // These are the operator columns:
          Table::Column(Column::kRowId, "rowid", ColumnType::kUlong, kHidden),
          Table::Column(Column::kQuantum, "quantum", ColumnType::kUlong,
                        kHidden),
          Table::Column(Column::kWindowStart, "window_start",
                        ColumnType::kUlong, kHidden),
          Table::Column(Column::kWindowDur, "window_dur", ColumnType::kUlong,
                        kHidden),
          // These are the ouput columns:
          Table::Column(Column::kTs, "ts", ColumnType::kUlong),
          Table::Column(Column::kDuration, "dur", ColumnType::kUlong),
          Table::Column(Column::kCpu, "cpu", ColumnType::kUint),
          Table::Column(Column::kQuantumTs, "quantum_ts", ColumnType::kUlong),
      },
      {Column::kRowId});
}

std::unique_ptr<Table::Cursor> WindowOperatorTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  uint64_t window_end = window_start_ + window_dur_;
  uint64_t step_size = quantum_ == 0 ? window_dur_ : quantum_;
  return std::unique_ptr<Table::Cursor>(
      new Cursor(this, window_start_, window_end, step_size, qc, argv));
}

int WindowOperatorTable::BestIndex(const QueryConstraints& qc,
                                   BestIndexInfo* info) {
  // Remove ordering on timestamp if it is the only ordering as we are already
  // sorted on TS. This makes span joining significantly faster.
  if (qc.order_by().size() == 1 && qc.order_by()[0].iColumn == Column::kTs &&
      !qc.order_by()[0].desc) {
    info->order_by_consumed = true;
  }
  return SQLITE_OK;
}

int WindowOperatorTable::Update(int argc,
                                sqlite3_value** argv,
                                sqlite3_int64*) {
  // We only support updates to ts and dur. Disallow deletes (argc == 1) and
  // inserts (argv[0] == null).
  if (argc < 2 || sqlite3_value_type(argv[0]) == SQLITE_NULL)
    return SQLITE_READONLY;

  quantum_ = static_cast<uint64_t>(sqlite3_value_int64(argv[3]));
  window_start_ = static_cast<uint64_t>(sqlite3_value_int64(argv[4]));
  window_dur_ = static_cast<uint64_t>(sqlite3_value_int64(argv[5]));

  return SQLITE_OK;
}

WindowOperatorTable::Cursor::Cursor(const WindowOperatorTable* table,
                                    uint64_t window_start,
                                    uint64_t window_end,
                                    uint64_t step_size,
                                    const QueryConstraints& qc,
                                    sqlite3_value** argv)
    : window_start_(window_start),
      window_end_(window_end),
      step_size_(step_size),
      table_(table) {
  current_ts_ = window_start_;

  // Set return first if there is a equals constraint on the row id asking to
  // return the first row.
  bool return_first = qc.constraints().size() == 1 &&
                      qc.constraints()[0].iColumn == Column::kRowId &&
                      IsOpEq(qc.constraints()[0].op) &&
                      sqlite3_value_int(argv[0]) == 0;
  // Set return CPU if there is an equals constraint on the CPU column.
  bool return_cpu = qc.constraints().size() == 1 &&
                    qc.constraints()[0].iColumn == Column::kCpu &&
                    IsOpEq(qc.constraints()[0].op);
  if (return_first) {
    filter_type_ = FilterType::kReturnFirst;
  } else if (return_cpu) {
    filter_type_ = FilterType::kReturnCpu;
    current_cpu_ = static_cast<uint32_t>(sqlite3_value_int(argv[0]));
  } else {
    filter_type_ = FilterType::kReturnAll;
  }
}

int WindowOperatorTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kQuantum: {
      sqlite3_result_int64(context,
                           static_cast<sqlite_int64>(table_->quantum_));
      break;
    }
    case Column::kWindowStart: {
      sqlite3_result_int64(context,
                           static_cast<sqlite_int64>(table_->window_start_));
      break;
    }
    case Column::kWindowDur: {
      sqlite3_result_int(context, static_cast<int>(table_->window_dur_));
      break;
    }
    case Column::kTs: {
      sqlite3_result_int64(context, static_cast<sqlite_int64>(current_ts_));
      break;
    }
    case Column::kDuration: {
      sqlite3_result_int64(context, static_cast<sqlite_int64>(step_size_));
      break;
    }
    case Column::kCpu: {
      sqlite3_result_int(context, static_cast<int>(current_cpu_));
      break;
    }
    case Column::kQuantumTs: {
      sqlite3_result_int64(context, static_cast<sqlite_int64>(quantum_ts_));
      break;
    }
    case Column::kRowId: {
      sqlite3_result_int64(context, static_cast<sqlite_int64>(row_id_));
      break;
    }
    default: {
      PERFETTO_FATAL("Unknown column %d", N);
      break;
    }
  }
  return SQLITE_OK;
}

int WindowOperatorTable::Cursor::Next() {
  switch (filter_type_) {
    case FilterType::kReturnFirst:
      current_ts_ = window_end_;
      break;
    case FilterType::kReturnCpu:
      current_ts_ += step_size_;
      quantum_ts_++;
      break;
    case FilterType::kReturnAll:
      if (++current_cpu_ == base::kMaxCpus && current_ts_ < window_end_) {
        current_cpu_ = 0;
        current_ts_ += step_size_;
        quantum_ts_++;
      }
      break;
  }
  row_id_++;
  return SQLITE_OK;
}

int WindowOperatorTable::Cursor::Eof() {
  return current_ts_ >= window_end_;
}

}  // namespace trace_processor
}  // namespace perfetto
