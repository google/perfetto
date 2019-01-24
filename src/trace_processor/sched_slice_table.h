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

#ifndef SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
#define SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_

#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/storage_table.h"

namespace perfetto {
namespace trace_processor {

// The implementation of the SQLite table containing slices of CPU time with the
// metadata for those slices.
class SchedSliceTable : public StorageTable {
 public:
  SchedSliceTable(sqlite3*, const TraceStorage* storage);

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  // StorageTable implementation.
  StorageSchema CreateStorageSchema() override;
  uint32_t RowCount() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

 private:
  uint32_t EstimateQueryCost(const QueryConstraints& cs);

  class EndStateColumn : public StorageColumn {
   public:
    EndStateColumn(std::string col_name,
                   const std::deque<ftrace_utils::TaskState>* deque);
    ~EndStateColumn() override;

    void ReportResult(sqlite3_context*, uint32_t row) const override;

    void Filter(int op, sqlite3_value*, FilteredRowIndex*) const override;

    Comparator Sort(const QueryConstraints::OrderBy&) const override;

    Table::ColumnType GetType() const override;

   private:
    static constexpr uint16_t kNumStateStrings =
        ftrace_utils::TaskState::kMaxState + 1;
    std::array<ftrace_utils::TaskState::TaskStateStr, kNumStateStrings>
        state_strings_;

    void FilterOnState(int op,
                       sqlite3_value* value,
                       FilteredRowIndex* index) const;

    const std::deque<ftrace_utils::TaskState>* deque_ = nullptr;
  };

  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
