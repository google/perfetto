/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_SCAN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_SCAN_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/range.h"

namespace perfetto::trace_processor::core::exec {

// Reads the rows of a dataframe which searches of its sorted columns keep.
//
// The general case, for plans with more than one search or a binary search:
// a plan with one equality search of a column whose search needs no type
// (an Id, a SetId sorted or a small value column) uses one of the operators in
// eq_scans.h instead, which dispatch on nothing per run.
//
// Rows a sorted column keeps for =, <, <=, > or >= are contiguous, so each
// search narrows a range of rows rather than testing them, comparing with one
// of the run's filter values (see Context::SetFilterValues). The range is
// what every search keeps, and is read as DataframeScan reads all the rows:
// the batches point at the dataframe's own storage.
//
// The searches run once per run, when the first batch is asked for, so a run
// which looks up one row does a few searches and points one batch at it.
//
// A search finds exactly the rows the dataframe interpreter's would, by the
// same kernels. Each column searched must be non-null and finalized, and the
// planner must have chosen the search for it (see SortedFilterStrategy).
class SortedScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  struct Search {
    enum class Kind : uint8_t {
      // Binary search of a column sorted ascending. An Id column's row is its
      // value, so it is not searched at all.
      kBinarySearch,
      // = on a Uint32 column sorted by SetId.
      kSetIdEq,
      // = on a column with SmallValueEq storage.
      kSmallValueEq,
    };
    std::shared_ptr<const dataframe::Column> column;
    Kind kind = Kind::kBinarySearch;
    // One of =, <, <=, >, >=. Only = for kSetIdEq and kSmallValueEq.
    Op op{Eq{}};
    // The filter value compared with.
    uint32_t value = 0;
  };

  // Reads `columns` of a dataframe of `row_count` rows. `pool` holds the
  // strings of the columns searched and must outlive the scan.
  SortedScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
             uint32_t row_count,
             std::vector<Search> searches,
             const StringPool* pool);
  ~SortedScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    ~State() override;
    RangeReader reader;
    const Context* context = nullptr;
    // Whether this run's searches have run.
    bool searched = false;
  };

  // A binary search, as filter::SearchSorted takes it, made once.
  struct Sorted {
    filter::SortedColumn column;
    RangeOp op{Eq{}};
    // Whether the value is a string compared by =, which arrives as its id.
    bool string_eq = false;
  };

  // The rows every search keeps, with the values in `context`.
  Range Find(const Context& context) const;

  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  uint32_t row_count_;
  std::vector<Search> searches_;
  // By search; used only by binary searches.
  std::vector<Sorted> sorted_;
  const StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_SCAN_H_
