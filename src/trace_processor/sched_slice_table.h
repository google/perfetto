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
  std::unique_ptr<Table::Cursor> CreateCursor(
      const QueryConstraints& query_constraints,
      sqlite3_value** argv) override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

 private:
  // Base class for other cursors, implementing column reporting.
  class BaseCursor : public Table::Cursor {
   public:
    BaseCursor(const TraceStorage* storage);
    virtual ~BaseCursor() override;

    virtual uint32_t RowIndex() = 0;
    int Column(sqlite3_context*, int N) override final;

   protected:
    const TraceStorage* const storage_;
  };

  // Very fast cursor which which simply increments through indices.
  class IncrementCursor : public BaseCursor {
   public:
    IncrementCursor(const TraceStorage*,
                    uint32_t min_idx,
                    uint32_t max_idx,
                    bool desc);

    int Next() override;
    uint32_t RowIndex() override;
    int Eof() override;

   private:
    uint32_t const min_idx_;
    uint32_t const max_idx_;
    bool const desc_;

    // In non-desc mode, this is an offset from min_idx while in desc mode, this
    // is an offset from max_idx_.
    uint32_t offset_ = 0;
  };

  // Reasonably fast cursor which stores a vector of booleans about whether
  // a row should be returned.
  class FilterCursor : public BaseCursor {
   public:
    FilterCursor(const TraceStorage*,
                 uint32_t min_idx,
                 uint32_t max_idx,
                 std::vector<bool> filter,
                 bool desc);

    int Next() override;
    uint32_t RowIndex() override;
    int Eof() override;

   private:
    void FindNext();

    uint32_t const min_idx_;
    uint32_t const max_idx_;
    std::vector<bool> filter_;
    bool const desc_;

    // In non-desc mode, this is an offset from min_idx while in desc mode, this
    // is an offset from max_idx_.
    uint32_t offset_ = 0;
  };

  // Slow path cursor which stores a sorted set of indices into storage.
  class SortedCursor : public BaseCursor {
   public:
    SortedCursor(const TraceStorage* storage,
                 uint32_t min_idx,
                 uint32_t max_idx,
                 const std::vector<QueryConstraints::OrderBy>&);
    SortedCursor(const TraceStorage* storage,
                 uint32_t min_idx,
                 uint32_t max_idx,
                 const std::vector<QueryConstraints::OrderBy>&,
                 const std::vector<bool>& filter);

    int Next() override;
    uint32_t RowIndex() override;
    int Eof() override;

   private:
    // Vector of row ids sorted by some order by constraints.
    std::vector<uint32_t> sorted_rows_;

    // An offset into |sorted_row_ids_| indicating the next row to return.
    uint32_t next_row_idx_ = 0;
  };

  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
