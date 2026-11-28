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
#include <utility>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

// TREE ORDER CHILD FIRST: puts tree rows into an order where every child
// precedes its parent. A fold up a tree runs over this order.
//
// Subtrees may be interleaved: this does not guarantee DFS post-order, so a
// fold up keeps a value per node rather than a stack of the current path.
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
class TreeChildFirst final : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    out->reads = {node_column_, parent_column_};
  }
  void SetUnreadColumns(core::ArenaSpan<bool> unread) override {
    unread_ = unread;
  }

  TreeChildFirst(uint32_t node_column, uint32_t parent_column);

 private:
  friend class PipelineFormatter;

  struct State : Breaker::State {
    // The rows, as they arrive.
    RowStore rows;
    // The node and parent of each row in order of arrival: what a sort needs.
    RunVector<uint32_t> nodes;
    RunVector<uint32_t> parents;

    // What is known of the tree so far.
    // By node number.
    RunBits has_row;
    uint32_t nodes_seen = 0;
    // Which orders the rows so far are still consistent with.
    bool parent_first = true;
    bool child_first = true;

    // The rows served.
    // The order to emit the rows in. Empty means in arrival order.
    RunVector<uint32_t> order;
    uint32_t emitted = 0;
  };

  OperatorState* NewSinkState(Context&) const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  bool Sort(State&) const;

  uint32_t node_column_;
  uint32_t parent_column_;
  // Input columns no later step reads (see Operator::SetUnreadColumns).
  core::ArenaSpan<bool> unread_;
};

// TREE ORDER PARENT FIRST: puts tree rows into an order where every parent
// precedes its children. A fold down a tree runs over this order.
//
// Not a breaker: a row can go out as soon as its parent has. Rows whose
// parent is already out stream straight through as views of their batch.
// Only a row arriving before its parent is held: retained and let go the
// moment the parent arrives, together with whatever is held under it. So the
// cost is proportional to how far out of order the input is, nothing for
// ordered input and everything for reversed input, and the planner never
// needs to know which. Rows once held stay retained until the next rewind.
//
// The order is parent first and nothing more: not a pre-order, so a fold
// down keeps a value per node rather than a path. The input columns are node
// numbers, which TreeNumberNodes produces. No column is added.
class TreeParentFirst final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    out->reads = {node_column_, parent_column_};
  }
  void SetUnreadColumns(core::ArenaSpan<bool> unread) override {
    unread_ = unread;
  }

  TreeParentFirst(uint32_t node_column, uint32_t parent_column);

  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;
  OpResult Finish(RowBatch& out, OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    // What is known about each node number.
    struct Nodes {
      // The node's row is out, or is on its way out in `letting_go`.
      RunBits out;
      // The first row of `held` waiting on this node, or kNoNode.
      RunVector<uint32_t> first_waiting;

      // Grows to at least `count` nodes, ahead of need.
      void Grow(uint32_t count, OperatorState& owner);
    };

    // Rows which arrived before their parent. Each knows its node and the
    // next row waiting on the same parent, which with `first_waiting` makes
    // a list per parent. A row let go stays here until none is waiting, or
    // the run ends.
    struct Waiting {
      RowStore rows;
      RunVector<uint32_t> node;
      RunVector<uint32_t> next_waiting;
      uint32_t let_go = 0;

      void Clear();
    };

    // The nodes, and the rows waiting on them.
    Nodes nodes;
    Waiting held;

    // Rows of `held` on their way out after the current batch, served a
    // batch at a time from `served`.
    RunVector<uint32_t> letting_go;
    uint32_t served = 0;

    // The batch's rows held, sliced out to be copied.
    RowBatch* held_batch = nullptr;

    // Scratch for one call: which of the batch's rows pass straight through,
    // and which are held.
    core::ArenaSpan<uint32_t> passing;
    core::ArenaSpan<uint32_t> holding;
  };

  OperatorState* NewState(Context&) const override;

  // Moves every row waiting on `node` to `letting_go`.
  void Release(uint32_t node, State&) const;
  OpResult LetGo(RowBatch& out, State&) const;

  uint32_t node_column_;
  uint32_t parent_column_;
  // Input columns no later step reads (see Operator::SetUnreadColumns).
  core::ArenaSpan<bool> unread_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_
