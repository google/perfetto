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

// TREE ORDER PARENT FIRST: puts tree rows into an order where every parent
// precedes its children. A fold down a tree runs over this order.
//
// Not a breaker: a row can go out as soon as its parent has. Rows whose
// parent is already out stream straight through as views of their batch.
// Only a row arriving before its parent is held: copied aside and let go the
// moment the parent arrives, together with whatever is held under it. So the
// cost is proportional to how far out of order the input is, nothing for
// ordered input and everything for reversed input, and the planner never
// needs to know which. Rows once held stay copied until the next rewind.
//
// The order is parent first and nothing more: not a pre-order, so a fold
// down keeps a value per node rather than a path. The input columns are node
// numbers, which TreeNumberNodes produces. No column is added.
class TreeParentFirst : public Operator {
 public:
  TreeParentFirst(uint32_t node_column, uint32_t parent_column);
  ~TreeParentFirst() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;
  OpResult Finish(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  struct State : OperatorState {
    ~State() override;

    // What is known about each node number.
    struct Nodes {
      uint32_t count = 0;
      BitVector has_row;
      // The node's row is out, or is on its way out in `letting_go`.
      BitVector out;
      // The first row of `held` waiting on this node, or kNoNode.
      FlexVector<uint32_t> first_waiting;

      void Grow(uint32_t count);
      void Clear();
    };

    // Rows which arrived before their parent. Each knows its node and the
    // next row waiting on the same parent, which with `first_waiting` makes
    // a list per parent. A row let go stays here, so this only grows until
    // the next rewind.
    struct Held {
      RowStore rows;
      FlexVector<uint32_t> node;
      FlexVector<uint32_t> next_waiting;
      uint32_t let_go = 0;

      void Clear();
    };

    Nodes nodes;
    Held held;

    // Rows of `held` on their way out after the current batch, served a
    // batch at a time from `served`.
    FlexVector<uint32_t> letting_go;
    uint32_t served = 0;

    // Scratch for one batch: which of its rows pass straight through, which
    // are held, and the held ones sliced out to be copied.
    FlexVector<uint32_t> passing;
    FlexVector<uint32_t> holding;
    RowBatch held_batch;

    base::Status status = base::OkStatus();
  };

  // Moves every row waiting on `node` to `letting_go`.
  void Release(uint32_t node, State&) const;
  OpResult LetGo(RowBatch& out, State&) const;

  uint32_t node_column_;
  uint32_t parent_column_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_
