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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_WALK_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_WALK_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

struct IntervalWalkSpec {
  enum class Source : uint8_t { kTs, kDur, kInput, kEdge };

  struct Output {
    Source source = Source::kInput;
    // Index in `input_retained_columns` (for kInput) or
    // `edges.retained_columns` (for kEdge).
    uint32_t index = 0;
  };

  // The input intervals to walk.
  IntervalLaneSpec input;
  // Columns of the input retained for kInput outputs.
  std::vector<uint32_t> input_retained_columns;

  // The step/edge relation, read whole before the input.
  // Its `lanes.key_columns` are the PER columns.
  CollectedInputSpec edges;
  // Indices in `edges.retained_columns` of the TO (step) columns, matching
  // `edges.lanes.key_columns` in count and order.
  std::vector<uint32_t> next_key_columns;

  std::vector<Output> outputs;
};

// INTERVAL WALK. For each input interval on a PER lane, intersects it with the
// intervals in `edges` on that lane and transitively follows the TO key to its
// target lane in `edges` until reaching a leaf interval (where the TO key is
// null, the target lane has no covering interval, or a cycle/depth limit is
// reached).
class IntervalWalk : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit IntervalWalk(IntervalWalkSpec);
  ~IntervalWalk() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

  struct State;

  bool EnsureEdgesRead(State&) const;
  base::Status ReadEdges(State&) const;
  base::Status WalkBatch(const RowBatch&, State&) const;
  void WalkRange(Ts start,
                 Ts end,
                 uint32_t lane,
                 uint32_t fallback_edge,
                 uint32_t in_row,
                 uint32_t depth,
                 State&) const;
  OpResult ServeResults(RowBatch& out, State&) const;

  IntervalWalkSpec spec_;
  bool has_edge_outputs_ = false;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_WALK_H_
