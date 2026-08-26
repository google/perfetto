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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_NUMBER_NODES_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_NUMBER_NODES_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// The parent of a root row.
inline constexpr uint32_t kNoNode = std::numeric_limits<uint32_t>::max();

// Appends node and parent-node Uint32 columns to a relation. Numbers are handed
// out densely from zero in order of first sighting.
//
// This is the only operator which has to know how a relation stores its ids:
// they can be of any width, and a filtered relation's ids are scattered over a
// wide range. Numbering them densely means an array indexed by node is the
// size of the input rather than of the table it was filtered from, and lets
// every operator downstream deal only in node numbers.
//
// A table scanned in row order with every parent an earlier row is the common
// case: the ids are the numbers, so nothing is looked up or checked.
class TreeNumberNodes : public Operator {
 public:
  TreeNumberNodes(uint32_t id_column, uint32_t parent_column);
  ~TreeNumberNodes() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

  // Whether every id seen so far was already its own node number.
  bool IsDenseForTesting(const OperatorState&) const;

 private:
  struct Key {
    int64_t value;
    bool is_string;

    bool operator==(const Key& other) const {
      return value == other.value && is_string == other.is_string;
    }
    template <typename H>
    friend H PerfettoHashValue(H h, const Key& key) {
      return H::Combine(std::move(h), key.value, key.is_string);
    }
  };
  struct State : OperatorState {
    ~State() override;
    // While the ids arriving are 0, 1, 2, ... they are already node numbers,
    // so the map stays empty.
    bool dense = true;
    uint32_t numbered = 0;
    base::FlatHashMap<Key, uint32_t> numbers;
    // Which nodes have had a row of their own, to catch an id used twice.
    BitVector has_row;
    // The batch's ids and parent ids, whatever type they arrived as.
    FlexVector<Variant> ids;
    FlexVector<Variant> parents;
    // The two columns appended to the batch.
    FlexVector<uint32_t> nodes;
    FlexVector<uint32_t> parent_nodes;
    base::Status status = base::OkStatus();
  };

  // Numbers a batch of a table scanned in row order whose parents all point
  // back, or returns false having changed nothing.
  bool NumberInOrder(const RowBatch&, uint32_t count, State&) const;
  bool NumberByKey(const RowBatch&, uint32_t count, State&) const;
  uint32_t Number(State&, const Variant& id) const;

  uint32_t id_column_;
  uint32_t parent_column_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_NUMBER_NODES_H_
