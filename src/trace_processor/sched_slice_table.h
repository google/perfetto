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

#include <sqlite3.h>
#include <limits>
#include <memory>

#include "perfetto/base/utils.h"
#include "src/trace_processor/query_constraints.h"
#include "src/trace_processor/table.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// The implementation of the SQLite table containing slices of CPU time with the
// metadata for those slices.
class SchedSliceTable : public Table {
 public:
  enum Column {
    kTimestamp = 0,
    kCpu = 1,
    kDuration = 2,
    kUtid = 3,
  };

  SchedSliceTable(sqlite3*, const TraceStorage* storage);

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  // Table implementation.
  Table::Schema CreateSchema(int argc, const char* const* argv) override;
  std::unique_ptr<Table::Cursor> CreateCursor() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

 private:
  // Transient state for a filter operation on a Cursor.
  class FilterState {
   public:
    FilterState(const TraceStorage* storage,
                const QueryConstraints& query_constraints,
                sqlite3_value** argv);

    void FindNextSlice();

    inline bool IsNextRowIdIndexValid() const {
      return next_row_id_index_ < sorted_row_ids_.size();
    }

    size_t next_row_id() const { return sorted_row_ids_[next_row_id_index_]; }

   private:
    // Updates |sorted_row_ids_| with the indices into the slices sorted by the
    // order by criteria.
    void SetupSortedRowIds(uint64_t min_ts, uint64_t max_ts);

    // Compares the slice at index |f| with the slice at index |s|on all
    // columns.
    // Returns -1 if the first slice is before the second in the ordering, 1 if
    // the first slice is after the second and 0 if they are equal.
    int CompareSlices(size_t f, size_t s);

    // Compares the slice at index |f| with the slice at index |s| on the
    // criteria in |order_by|.
    // Returns -1 if the first slice is before the second in the ordering, 1 if
    // the first slice is after the second and 0 if they are equal.
    int CompareSlicesOnColumn(size_t f,
                              size_t s,
                              const QueryConstraints::OrderBy& order_by);

    void FindNextRowAndTimestamp();

    // Vector of row ids sorted by the the given order by constraints.
    std::vector<uint32_t> sorted_row_ids_;

    // Bitset for filtering slices.
    std::vector<bool> row_filter_;

    // An offset into |sorted_row_ids_| indicating the next row to return.
    uint32_t next_row_id_index_ = 0;

    // The sorting criteria for this filter operation.
    std::vector<QueryConstraints::OrderBy> order_by_;

    const TraceStorage* const storage_;
  };

  // Implementation of the SQLite cursor interface.
  class Cursor : public Table::Cursor {
   public:
    Cursor(const TraceStorage* storage);

    // Implementation of Table::Cursor.
    int Filter(const QueryConstraints&, sqlite3_value**) override;
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context*, int N) override;

   private:
    const TraceStorage* const storage_;
    std::unique_ptr<FilterState> filter_state_;
  };

  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
