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

// Which columns say where a row sits in the tree, and what is being summed.
struct AccumulateSpec {
  // Node numbers, which TREE NUMBER NODES is what makes.
  uint32_t node_column = 0;
  uint32_t parent_column = 1;
  uint32_t value_column = 2;
};

// What one execution of an accumulate is carrying between batches: a running
// total per node, and this batch's answers.
class AccumulateState : public OperatorState {
 public:
  ~AccumulateState() override;

  std::vector<int64_t> by_node;
  std::shared_ptr<FlexVector<int64_t>> totals =
      std::make_shared<FlexVector<int64_t>>();
  base::Status status = base::OkStatus();
};

// TREE ACCUMULATE UP: a node's total is its own value plus everything below
// it. Reads child first, so a node's descendants have all gone past by the
// time it arrives and its total is settled on sight.
//
// Nothing is buffered: the rows leave as they come, and what is carried
// between batches is one running total per node.
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

// TREE ACCUMULATE DOWN: a node's total is its own value plus everything above
// it. Reads parent first, for the same reason the other way up.
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
