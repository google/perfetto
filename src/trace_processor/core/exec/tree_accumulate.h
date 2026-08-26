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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// The columns holding the tree structure and the values being summed. Node and
// parent columns must be flat, non-null Uint32 columns; values must be flat
// Int64. A null value contributes zero.
struct AccumulateSpec {
  uint32_t node_column = 0;
  uint32_t parent_column = 1;
  uint32_t value_column = 2;
};

// What one execution carries between batches: a running total per node, and
// the totals computed for the current batch.
class AccumulateState : public OperatorState {
 public:
  ~AccumulateState() override;

  std::vector<int64_t> by_node;
  std::vector<uint32_t> node_scratch;
  std::vector<uint32_t> parent_scratch;
  std::vector<int64_t> value_scratch;
  std::shared_ptr<FlexVector<int64_t>> totals =
      std::make_shared<FlexVector<int64_t>>();
  base::Status status = base::OkStatus();
};

// Sums each node's value with the values of everything below it.
//
// Requires rows child first, so every descendant has been seen when the node
// arrives. Input columns are preserved and one flat Int64 total column is
// appended.
class TreeAccumulateUp : public Operator {
 public:
  explicit TreeAccumulateUp(AccumulateSpec);
  ~TreeAccumulateUp() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  AccumulateSpec spec_;
};

// Sums each node's value with the values of everything above it.
//
// Requires rows parent first, so every ancestor has been totalled when the node
// arrives. Input columns are preserved and one flat Int64 total column is
// appended.
class TreeAccumulateDown : public Operator {
 public:
  explicit TreeAccumulateDown(AccumulateSpec);
  ~TreeAccumulateDown() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  AccumulateSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_
