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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_FROM_INTERVALS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_FROM_INTERVALS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

struct TreeFromIntervalsSpec {
  enum class Source : uint8_t { kTs, kDur, kId, kParentId, kInput };

  struct Output {
    Source source = Source::kInput;
    // Index in `retained_columns` (for kInput).
    uint32_t index = 0;
  };

  // The input intervals and their `parent` equality key columns.
  IntervalLaneSpec input;
  // Columns of the input retained for kInput outputs.
  std::vector<uint32_t> retained_columns;
  // Positions in the input batch of the `child` equality key columns, matching
  // `input.key_columns` in count and order.
  std::vector<uint32_t> next_key_columns;
  std::vector<Output> outputs;
};

// TREE FROM INTERVALS ON <expr>. Connects each `child` interval to the
// overlapping `parent` interval matching `<expr>` (splitting `child` at
// boundaries where its resolved parent chain transitions) and outputs rows
// parents before their children: the configured outputs (`ts`, `dur`, `id` as
// Id, `parent_id` as Uint32 null for a root, and the retained input columns),
// followed by two Uint32 columns numbering the rows out from zero, of the row
// and of its parent's row (kNoNode for a root). The numbers are what the tree
// operators read, as though from TreeNumberNodes.
class TreeFromIntervals : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit TreeFromIntervals(TreeFromIntervalsSpec);
  ~TreeFromIntervals() override;

 private:
  friend class PipelineFormatter;

  struct State;

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  TreeFromIntervalsSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_FROM_INTERVALS_H_
