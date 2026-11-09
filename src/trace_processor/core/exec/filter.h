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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/util/glob.h"

namespace perfetto::trace_processor::core::exec {

// Keeps the rows of each batch for which every condition holds. Nothing is
// copied: the batch's selection is narrowed.
//
// A condition compares a column with one of the run's params (see
// Context::SetParams) by =, !=, <, <=, >, >=, IN or, for a string
// column, GLOB, or tests it with IS [NOT] NULL. The values come already cast
// for the column and comparison, so a Filter only compares.
//
// A condition means exactly what the same filter on a dataframe means: no
// comparison is true of a null, and an integer column compared with 2.5 keeps
// the rows SQL would.
//
// Columns must be flat or Id sequences, of the storage type their values were
// cast for. IS [NOT] NULL also reads variant columns.
class Filter final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    for (const Condition& condition : conditions_) {
      out->reads.push_back(condition.column);
      out->reads_sparse.push_back(condition.column);
    }
  }

  struct Condition {
    uint32_t column = 0;
    Op op{Eq{}};
    // The param compared with. Unused by IS [NOT] NULL.
    uint32_t param = 0;
  };

  // A GLOB pattern, compiled, and what has been matched with it. A string's
  // result depends only on the pattern, so all of it is kept until the
  // pattern changes.
  struct GlobMemo {
    std::string pattern;
    std::optional<util::GlobMatcher> matcher;
    // Rows matched one at a time with the pattern.
    uint64_t rows = 0;
    // The result of every string with an id below `matched_below`, once the
    // pool has been matched. Strings interned after are above it.
    BitVector matches;
    uint32_t matched_below = 0;
  };

  // `pool` holds the strings of the columns, and must outlive the filter.
  Filter(std::vector<Condition> conditions, const StringPool* pool);
  ~Filter() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    explicit State(const Context& c) : context(&c) {}
    ~State() override;
    // Where the run's params are.
    const Context* context;
    // The batch rows still kept, increasing.
    CallScratch<std::array<uint32_t, kMaxBatchRows>> rows;
    // By batch row, the values of a sparse column at the rows kept.
    CallScratch<std::array<uint64_t, kMaxBatchRows>> sparse_values;
    // By condition, so that each string is matched against a GLOB once. Only
    // a filter with a GLOB has them.
    std::vector<GlobMemo> globs;
  };

  std::vector<Condition> conditions_;
  const StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
