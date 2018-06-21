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

#include <limits>
#include <memory>

#include "sqlite3.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// The implementation of the SQLite table containing slices of CPU time with the
// metadata for those slices.
class SchedSliceTable {
 public:
  enum Column { kTimestamp = 0, kCpu = 1, kDuration = 2 };
  struct OrderBy {
    Column column = kTimestamp;
    bool desc = false;
  };

  SchedSliceTable(const TraceStorage* storage);
  static sqlite3_module CreateModule();

  // Implementation for sqlite3_vtab.
  int BestIndex(sqlite3_index_info* index_info);
  int Open(sqlite3_vtab_cursor** ppCursor);

 private:
  using Constraint = sqlite3_index_info::sqlite3_index_constraint;

  struct IndexInfo {
    std::vector<OrderBy> order_by;
    std::vector<Constraint> constraints;
  };

  class Cursor {
   public:
    Cursor(SchedSliceTable* table, const TraceStorage* storage);

    // Implementation of sqlite3_vtab_cursor.
    int Filter(int idxNum, const char* idxStr, int argc, sqlite3_value** argv);
    int Next();
    int Eof();

    int Column(sqlite3_context* context, int N);
    int RowId(sqlite_int64* pRowid);

   private:
    class PerCpuState {
     public:
      bool IsNextRowIdIndexValid() const {
        return next_row_id_index_ < sorted_row_ids_.size();
      }

      uint32_t next_row_id_index() const { return next_row_id_index_; }

      void set_next_row_id_index(uint32_t index) {
        PERFETTO_DCHECK(next_row_id_index_ < sorted_row_ids_.size());
        next_row_id_index_ = index;
      }

      std::vector<uint32_t>* sorted_row_ids() { return &sorted_row_ids_; }

      size_t next_row_id() const { return sorted_row_ids_[next_row_id_index_]; }

     private:
      // Vector of row ids sorted by the the given order by constraints.
      std::vector<uint32_t> sorted_row_ids_;

      // An offset into |sorted_row_ids_| indicating the next row to return.
      uint32_t next_row_id_index_ = 0;
    };

    class FilterState {
     public:
      FilterState(const TraceStorage* storage,
                  std::vector<OrderBy> order_by,
                  std::vector<Constraint> constraints,
                  sqlite3_value** argv);

      void FindCpuWithNextSlice();
      bool IsNextCpuValid() const { return next_cpu_ < per_cpu_state_.size(); }
      PerCpuState* StateForCpu(uint32_t cpu) { return &per_cpu_state_[cpu]; }

      uint32_t next_cpu() const { return next_cpu_; }

     private:
      // Compares the next slice of the given |cpu| with the next slice of the
      // |next_cpu_|. Return <0 if |cpu| is ordered before, >0 if ordered after,
      // and 0 if they are equal.
      int CompareCpuToNextCpu(uint32_t cpu);

      const TraceStorage* const storage_;

      // One entry for each cpu which is used in filtering.
      std::array<PerCpuState, TraceStorage::kMaxCpus> per_cpu_state_;
      uint32_t next_cpu_ = 0;

      std::vector<OrderBy> order_by_;
    };

    sqlite3_vtab_cursor base_;  // Must be first.

    SchedSliceTable* const table_;
    const TraceStorage* const storage_;

    std::unique_ptr<FilterState> filter_state_;
  };

  static inline Cursor* AsCursor(sqlite3_vtab_cursor* cursor) {
    return reinterpret_cast<Cursor*>(cursor);
  }

  sqlite3_vtab base_;  // Must be first.
  const TraceStorage* const storage_;

  // One entry for each BestIndex call.
  std::vector<IndexInfo> indexes_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
