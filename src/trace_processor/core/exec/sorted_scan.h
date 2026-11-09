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

// Searches of a dataframe's sorted columns. Each narrows a contiguous range of
// rows with the search kernels of core/common (see filter_kernels.h).
// Searched columns must be non-null and finalized.
class SortedSearches {
 public:
  struct Search {
    enum class Kind : uint8_t {
      // Binary search of an ascending column; an Id column needs no search.
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
    // Index into the run's params.
    uint32_t param = 0;
  };

  // `pool` must outlive the searches.
  SortedSearches(std::vector<Search> searches, const StringPool* pool);
  ~SortedSearches();

  // The rows of `rows` which every search keeps, for the run's params.
  Range Find(const Context& context, Range rows) const;

  bool empty() const { return searches_.empty(); }

 private:
  friend class PipelineFormatter;

  // Arguments to filter::SearchSorted, built once.
  struct Sorted {
    filter::SortedColumn column;
    RangeOp op{Eq{}};
    // String = values arrive as interned ids.
    bool string_eq = false;
  };

  Range FindAll(const Context& context, Range rows) const;
  Range FindIdEq(const Context& context, Range rows) const;

  std::vector<Search> searches_;
  // Parallel to searches_; used only by binary searches.
  std::vector<Sorted> sorted_;
  const StringPool* pool_;
  // The hottest lookup, id = ?, skips the general search loop.
  bool only_id_eq_ = false;
};

// Reads the rows of a dataframe kept by searches of its sorted columns. The
// searches run once per run, on the first GetData(); the range is then read
// like DataframeScan.
class SortedScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  using Search = SortedSearches::Search;

  // `pool` must outlive the scan.
  SortedScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
             uint32_t row_count,
             std::vector<Search> searches,
             const StringPool* pool);
  ~SortedScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  RowBatch* Start(RowBatch& scratch, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    ~State() override;
    RangeReader reader;
    const Context* context = nullptr;
    bool searched = false;
  };

  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  uint32_t row_count_;
  SortedSearches searches_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_SCAN_H_
