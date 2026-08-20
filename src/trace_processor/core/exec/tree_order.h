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
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/owned_column.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// Which way round a stream of tree rows is.
//
// A fold up a tree and a fold down it want opposite orders, and each is a
// single streaming pass in the one it wants. Neither order is the right one;
// which to be in is a question about what comes next.
enum class TreeRowOrder : uint8_t {
  // Every parent precedes its children. What a fold downwards reads.
  kParentFirst,
  // Every child precedes its parent. What a fold upwards reads.
  kChildFirst,
};

// Puts a stream of rows into a tree order and says where each row's parent is.
//
// Streams when the rows already arrive in the order asked for, inverts when
// they arrive in the other, and sorts when they arrive in neither. Which of
// the three it does is the whole cost of the operator, so it says which, and
// takes what the planner knows rather than guessing: an operator cannot start
// emitting and change its mind, so `input` is the difference between passing
// rows through and holding all of them.
//
// Two columns are appended: the row's own node number, and its parent's, or
// -1 for a root. They are numbered densely from zero, which is what makes an
// array indexed by node the right size when the ids arriving are a filtered
// scattering of some table's. Ids that already arrive dense and in order are
// left alone and cost nothing.
class TreeOrder : public Source {
 public:
  struct Spec {
    // The columns carrying identity and parentage. A null parent is a root.
    uint32_t id_column = 0;
    uint32_t parent_column = 1;

    // How many columns arrive. Needed up front so that where the appended
    // ones land is answerable before any row has been read, which is when
    // whatever consumes them is built.
    uint32_t input_columns = 2;

    // The order to put the rows in.
    TreeRowOrder want = TreeRowOrder::kParentFirst;

    // The order the rows arrive in, when the planner knows it. Without it the
    // rows have to be held, because by the time an out-of-order row proves
    // the guess wrong the rows before it have already gone.
    std::optional<TreeRowOrder> input;
  };

  TreeOrder(Source& input, Spec spec);
  ~TreeOrder() override;

  void Reset() override;
  RowBatch* Next() override;
  base::Status status() const override { return status_; }

  // Where the two appended columns are in the batches this hands out.
  uint32_t node_column() const { return spec_.input_columns; }
  uint32_t parent_ordinal_column() const { return spec_.input_columns + 1; }

  // Whether the rows were passed through rather than held. For tests and for
  // anything that wants to explain where a query's memory went.
  bool streamed() const { return mode_ == Mode::kStreaming; }

 private:
  enum class Mode : uint8_t {
    // The planner said the rows already arrive in the order wanted.
    kStreaming,
    // Everything else: hold the rows, then work out how to hand them back.
    kBuffering,
  };

  // Reads every row, holding what it reads. Sets `status_` and returns false
  // if the rows are not a tree.
  bool Fill();
  bool Consume(RowBatch& batch, bool retain);
  RowBatch* NextStreaming();
  RowBatch* NextBuffered();

  // Works out an order in which every parent precedes its children. False,
  // with `status_` set, if the rows are not a tree but a cycle.
  bool Sort();

  // The node number for `id`, assigning one if this is the first sighting.
  uint32_t Number(int64_t id);

  // Marks `node` as having its row at `row`, and checks that row's place
  // against what both orders require.
  void Note(uint32_t node, int64_t parent, uint32_t row);

  Source& input_;
  Spec spec_;
  Mode mode_;
  base::Status status_ = base::OkStatus();

  // Node numbering. While the ids arriving are exactly 0, 1, 2, ... they are
  // already node numbers and the map stays empty.
  bool dense_ = true;
  uint32_t numbered_ = 0;
  base::FlatHashMap<int64_t, uint32_t> numbers_;

  // Which nodes have had a row and where it was, by node number, and whether
  // each order still holds for every row seen so far.
  BitVector has_row_;
  std::vector<uint32_t> row_of_node_;
  bool parent_first_ = true;
  bool child_first_ = true;

  // The appended columns' values, for every row in order of arrival.
  std::vector<int64_t> nodes_;
  std::vector<int64_t> parents_;

  // Streaming: one batch's worth, refilled on every pull.
  std::vector<int64_t> batch_nodes_;
  std::vector<int64_t> batch_parents_;

  // Buffering: the whole input, one column at a time, and the order to read
  // it back in. An empty order means straight through.
  std::vector<std::unique_ptr<OwnedColumn>> store_;
  std::vector<uint32_t> order_;
  RowBatch batch_;
  uint32_t total_ = 0;
  uint32_t emitted_ = 0;
  bool filled_ = false;
};

// TREE ORDER PARENT FIRST: every parent precedes its children.
class TreeParentFirst : public TreeOrder {
 public:
  TreeParentFirst(Source& input,
                  uint32_t id_column,
                  uint32_t parent_column,
                  uint32_t input_columns,
                  std::optional<TreeRowOrder> arriving = std::nullopt);
  ~TreeParentFirst() override;
};

// TREE ORDER CHILD FIRST: every child precedes its parent.
class TreeChildFirst : public TreeOrder {
 public:
  TreeChildFirst(Source& input,
                 uint32_t id_column,
                 uint32_t parent_column,
                 uint32_t input_columns,
                 std::optional<TreeRowOrder> arriving = std::nullopt);
  ~TreeChildFirst() override;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ORDER_H_
