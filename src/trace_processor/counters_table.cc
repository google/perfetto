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

}  // namespace

CountersTable::CountersTable(const TraceStorage* storage) : storage_(storage) {}

void CountersTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<CountersTable>(db, storage,
                                 "CREATE TABLE counters("
                                 "ts UNSIGNED BIG INT, "
                                 "name text, "
                                 "value UNSIGNED BIG INT, "
                                 "dur UNSIGNED BIG INT, "
                                 "ref UNSIGNED INT, "
                                 "reftype TEXT, "
                                 "PRIMARY KEY(name, ts, ref)"
                                 ") WITHOUT ROWID;");
}

std::unique_ptr<Table::Cursor> CountersTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int CountersTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost = 10;

  // If the query has a constraint on the |kRef| field, return a reduced cost
  // because we can do that filter efficiently.
  const auto& constraints = qc.constraints();
  if (constraints.size() == 1 && constraints.front().iColumn == Column::kRef) {
    info->estimated_cost = IsOpEq(constraints.front().op) ? 1 : 10;
  }

  return SQLITE_OK;
}

CountersTable::Cursor::Cursor(const TraceStorage* storage)
    : storage_(storage) {}

int CountersTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kTimestamp: {
      const auto& freq = storage_->GetFreqForCpu(current_cpu_);
      sqlite3_result_int64(context,
                           static_cast<int64_t>(freq[index_in_cpu_].first));
      break;
    }
    case Column::kValue: {
      const auto& freq = storage_->GetFreqForCpu(current_cpu_);
      sqlite3_result_int64(context, freq[index_in_cpu_].second);
      break;
    }
    case Column::kName: {
      sqlite3_result_text(context, "cpufreq", -1, nullptr);
      break;
    }
    case Column::kRef: {
      sqlite3_result_int64(context, current_cpu_);
      break;
    }
    case Column::kRefType: {
      sqlite3_result_text(context, "cpu", -1, nullptr);
      break;
    }
    case Column::kDuration: {
      const auto& freq = storage_->GetFreqForCpu(current_cpu_);
      uint64_t duration = 0;
      if (index_in_cpu_ + 1 < freq.size()) {
        duration = freq[index_in_cpu_ + 1].first - freq[index_in_cpu_].first;
      }
      sqlite3_result_int64(context, static_cast<int64_t>(duration));
      break;
    }
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int CountersTable::Cursor::Filter(const QueryConstraints& qc,
                                  sqlite3_value** argv) {
  for (size_t j = 0; j < qc.constraints().size(); j++) {
    const auto& cs = qc.constraints()[j];
    if (cs.iColumn == Column::kRef) {
      auto constraint_cpu = static_cast<uint32_t>(sqlite3_value_int(argv[j]));
      if (IsOpEq(cs.op)) {
        filter_by_cpu_ = true;
        filter_cpu_ = constraint_cpu;
      }
    }
  }

  return SQLITE_OK;
}

int CountersTable::Cursor::Next() {
  if (filter_by_cpu_) {
    current_cpu_ = filter_cpu_;
    ++index_in_cpu_;
  } else {
    if (index_in_cpu_ < storage_->GetFreqForCpu(current_cpu_).size() - 1) {
      index_in_cpu_++;
    } else if (current_cpu_ < storage_->GetMaxCpu()) {
      ++current_cpu_;
      index_in_cpu_ = 0;
    }
    // If the cpu is has no freq events, move to the next one.
    while (current_cpu_ != storage_->GetMaxCpu() &&
           storage_->GetFreqForCpu(current_cpu_).size() == 0) {
      ++current_cpu_;
    }
  }
  return SQLITE_OK;
}

int CountersTable::Cursor::Eof() {
  if (filter_by_cpu_) {
    return index_in_cpu_ == storage_->GetFreqForCpu(current_cpu_).size();
  }
  return current_cpu_ == storage_->GetMaxCpu();
}

}  // namespace trace_processor
}  // namespace perfetto
