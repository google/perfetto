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
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// The order the rows of a tree arrive in. Folding up a tree and folding down
// it need opposite orders.
enum class TreeRowOrder : uint8_t {
  kParentFirst,
  kChildFirst,
};

// Puts a stream of tree rows into the requested order.
//
// Rows already in that order stream straight through; rows in the opposite
// order are reversed; rows in neither are topologically sorted, which means
// buffering all of them. Which of the three happens is the entire cost of this
// operator, so the planner tells it the input's order rather than letting it
// guess: once rows have started being emitted it is too late to change course.
//
// The input columns are node numbers, which TreeNumberNodes produces. No
// column is added.
class TreeOrder : public Source {
 public:
  struct Spec {
    uint32_t node_column = 0;
    uint32_t parent_column = 1;
    TreeRowOrder want = TreeRowOrder::kParentFirst;
    // The order the planner knows the input to be in. Without it, all the
    // rows have to be buffered.
    std::optional<TreeRowOrder> input;
  };

  TreeOrder(const Source& input, Spec spec);
  ~TreeOrder() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

  bool streamed() const { return mode_ == Mode::kStreaming; }

 private:
  enum class Mode : uint8_t { kStreaming, kBuffering };

  struct State : OperatorState {
    ~State() override;

    std::unique_ptr<OperatorState> input;
    RowBatch input_batch;
    base::Status status = base::OkStatus();

    // By node number.
    BitVector has_row;
    // Only built when buffering: a sort needs it, streaming does not.
    std::vector<uint32_t> row_of_node;
    bool parent_first = true;
    bool child_first = true;
    uint32_t nodes_seen = 0;

    // The node and parent of each row, in order of arrival. Only built when
    // buffering, for the same reason as `row_of_node`.
    std::vector<uint32_t> nodes;
    std::vector<uint32_t> parents;

    // Reused across batches so gathering a selection allocates only once.
    std::vector<uint32_t> node_scratch;
    std::vector<uint32_t> parent_scratch;

    RowStore rows;
    // The order to emit the buffered rows in. Empty means in arrival order.
    std::vector<uint32_t> order;
    uint32_t emitted = 0;
    bool filled = false;
  };

  bool Fill(State&) const;
  bool Consume(RowBatch& batch, State&, bool buffering) const;
  bool Sort(State&) const;
  void Note(State&,
            uint32_t node,
            uint32_t parent,
            uint32_t row,
            bool buffering) const;

  const Source& input_;
  Spec spec_;
  Mode mode_;
};

// TREE ORDER PARENT FIRST: every parent precedes its children.
class TreeParentFirst : public TreeOrder {
 public:
  TreeParentFirst(const Source& input,
                  uint32_t node_column,
                  uint32_t parent_column,
                  std::optional<TreeRowOrder> arriving = std::nullopt);
  ~TreeParentFirst() override;
};

// TREE ORDER CHILD FIRST: every child precedes its parent.
class TreeChildFirst : public TreeOrder {
 public:
  TreeChildFirst(const Source& input,
                 uint32_t node_column,
                 uint32_t parent_column,
                 std::optional<TreeRowOrder> arriving = std::nullopt);
  ~TreeChildFirst() override;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_
