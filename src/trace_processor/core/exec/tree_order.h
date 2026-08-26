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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_

#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// TREE ORDER CHILD FIRST: puts tree rows into a depth first post-order, so
// every child precedes its parent. A fold up a tree runs over this order.
//
// A breaker: the first row out has to be a leaf, and no row is known to be a
// leaf until the input ends. Rows which arrived child first come back as they
// arrived; rows which arrived parent first are reversed; rows in neither
// order are sorted. Which of the three happens is found out from the rows,
// so the planner only needs to leave this out when it knows the input is
// already child first.
//
// The input columns are node numbers, which TreeNumberNodes produces. No
// column is added.
class TreeChildFirst : public Breaker {
 public:
  TreeChildFirst(const Source& input,
                 uint32_t node_column,
                 uint32_t parent_column);
  TreeChildFirst(Source&&, uint32_t, uint32_t) = delete;
  ~TreeChildFirst() override;

  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finish(Breaker::State& state) const override;

 private:
  struct State : Breaker::State {
    ~State() override;

    // By node number.
    BitVector has_row;
    uint32_t nodes_seen = 0;
    // Which orders the rows so far are still consistent with.
    bool parent_first = true;
    bool child_first = true;

    // The node and parent of each row in order of arrival, and each node's
    // row: what a sort needs.
    FlexVector<uint32_t> nodes;
    FlexVector<uint32_t> parents;
    FlexVector<uint32_t> row_of_node;

    RowStore rows;
    // The order to emit the rows in. Empty means in arrival order.
    FlexVector<uint32_t> order;
    uint32_t emitted = 0;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;
  void Reset(Breaker::State& state) const override;

  bool Sort(State&) const;

  uint32_t node_column_;
  uint32_t parent_column_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_
