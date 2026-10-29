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
#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"

namespace perfetto::trace_processor::core::exec {

// Keeps the rows of each batch for which every condition holds. Nothing is
// copied: the batch's selection is narrowed.
//
// A condition compares a column with one of the run's filter values (see
// Context::SetFilterValues) by =, !=, <, <=, >, >=, IN or, for a string
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

  struct Condition {
    uint32_t column = 0;
    Op op{Eq{}};
    // The filter value compared with. Unused by IS [NOT] NULL.
    uint32_t value = 0;
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
    // Where the run's filter values are.
    const Context* context;
    // The batch rows still kept, increasing.
    std::array<uint32_t, kMaxBatchRows> rows;
  };

  std::vector<Condition> conditions_;
  const StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
