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
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// The parent of a root row.
inline constexpr uint32_t kNoNode = std::numeric_limits<uint32_t>::max();

// Replaces a relation's ids and parent ids with node numbers, handed out
// densely from zero in order of first sighting.
//
// This is the only operator which has to know how a relation stores its ids:
// they can be of any width, and a filtered relation's ids are scattered over a
// wide range. Numbering them densely means an array indexed by node is the
// size of the input rather than of the table it was filtered from, and lets
// every operator downstream deal only in node numbers.
class TreeNumberNodes : public Operator {
 public:
  TreeNumberNodes(uint32_t id_column, uint32_t parent_column);
  ~TreeNumberNodes() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  struct Numbers {
    std::vector<uint32_t> nodes;
    std::vector<uint32_t> parents;
  };
  struct State : OperatorState {
    ~State() override;
    // While the ids arriving are 0, 1, 2, ... they are already node numbers,
    // so the map stays empty.
    bool dense = true;
    uint32_t numbered = 0;
    base::FlatHashMap<int64_t, uint32_t> numbers;
    std::shared_ptr<Numbers> out = std::make_shared<Numbers>();
    std::vector<int64_t> id_keys;
    std::vector<int64_t> parent_keys;
    std::vector<uint8_t> parent_null;
    base::Status status = base::OkStatus();
  };

  uint32_t Number(State&, int64_t key) const;

  uint32_t id_column_;
  uint32_t parent_column_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_NUMBER_NODES_H_
