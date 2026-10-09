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

#include "src/trace_processor/core/exec/tree_accumulate.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// What one execution carries between batches: the aggregates' state for each
// node seen.
class AccumulateState : public OperatorState {
 public:
  AccumulateState(Context& c, size_t aggregates)
      : OperatorState(ResetEachRun{}), context(&c) {
    seen.resize(aggregates);
  }
  ~AccumulateState() override;
  void Reset() override {
    states.clear();
    for (SeenBytes& bytes : seen) {
      bytes.Clear();
    }
    status = base::OkStatus();
  }

  // By node, `stride` words apart: each aggregate's state in turn.
  FlexVector<int64_t> states;
  // By aggregate: see GroupStates::seen.
  std::vector<SeenBytes> seen;
  std::vector<uint32_t> node_scratch;
  std::vector<uint32_t> parent_scratch;
  // The edges of the batch's rows with a parent, in row order.
  FlexVector<GroupMerge> merges;
  AggregateInputLoader input;
  // The rows kept's results, before they're written at the batch's rows.
  FlexVector<int64_t> values;
  BitVector valid;
  // What each batch's columns are written in.
  Context* context;
  base::Status status = base::OkStatus();
};

AccumulateState::~AccumulateState() = default;

// The column's values at the rows kept, laid out flat, gathering once unless
// the rows kept are the first ones.
const uint32_t* Flatten(const ColumnView& column,
                        const Selection& selection,
                        std::vector<uint32_t>* scratch) {
  const auto* data =
      static_cast<const uint32_t*>(column.data()) + column.start();
  if (selection.prefix()) {
    return data;
  }
  scratch->resize(selection.size());
  for (uint32_t row = 0; row < selection.size(); ++row) {
    (*scratch)[row] = data[selection[row]];
  }
  return scratch->data();
}

bool IsNodeColumn(const ColumnView& column) {
  return column.kind() == ColumnView::Kind::kFlat &&
         column.type().Is<Uint32>() && column.validity() == nullptr;
}

std::vector<std::unique_ptr<AggregateFunction>> MakeFunctions(
    const TreeAccumulateSpec& spec) {
  std::vector<std::unique_ptr<AggregateFunction>> functions;
  for (const AggregateCall& call : spec.aggregates) {
    functions.push_back(MakeAggregateFunction(call.function));
  }
  return functions;
}

// Folds a batch of rows along the tree, appending each aggregate's column:
// up adds each node's state into its parent's, rows child first; down adds
// each parent's state into its node's, rows parent first. Either way every
// row's node is complete by the time the batch is done.
bool Accumulate(
    const TreeAccumulateSpec& spec,
    const std::vector<std::unique_ptr<AggregateFunction>>& functions,
    bool up,
    RowBatch& batch,
    AccumulateState& s) {
  const ColumnView& node_column = batch.column(spec.node_column);
  const ColumnView& parent_column = batch.column(spec.parent_column);
  if (!IsNodeColumn(node_column) || !IsNodeColumn(parent_column)) {
    s.status = base::ErrStatus(
        "TREE ACCUMULATE: node columns must be non-null Uint32");
    return false;
  }
  const Selection& selection = batch.selection();
  uint32_t count = batch.size();
  const uint32_t* nodes = Flatten(node_column, selection, &s.node_scratch);
  const uint32_t* parents =
      Flatten(parent_column, selection, &s.parent_scratch);

  // Makes room for every node the batch names, and lists its edges.
  uint32_t stride = 0;
  for (const auto& function : functions) {
    stride += function->state_words();
  }
  uint64_t nodes_needed = s.states.size() / std::max(stride, 1u);
  s.merges.clear();
  for (uint32_t row = 0; row < count; ++row) {
    nodes_needed = std::max<uint64_t>(nodes_needed, uint64_t{nodes[row]} + 1);
    if (parents[row] != kNoNode) {
      nodes_needed =
          std::max<uint64_t>(nodes_needed, uint64_t{parents[row]} + 1);
      s.merges.push_back(up ? GroupMerge{parents[row], nodes[row]}
                            : GroupMerge{nodes[row], parents[row]});
    }
  }
  s.states.push_back_multiple(0, nodes_needed * stride - s.states.size());

  bool overflow = false;
  uint32_t offset = 0;
  for (uint32_t a = 0; a < functions.size(); ++a) {
    const AggregateFunction& function = *functions[a];
    AggregateInput input;
    input.rows = count;
    if (function.reads_input() &&
        !s.input.Load(batch.column(spec.aggregates[a].column), selection,
                      nullptr, count, &input)) {
      s.status = base::ErrStatus("TREE ACCUMULATE: values must be Int64");
      return false;
    }
    GroupStates states{s.states.data() + offset, stride};
    offset += function.state_words();
    if (function.tracks_seen()) {
      states.seen = s.seen[a].For(static_cast<uint32_t>(nodes_needed),
                                  input.valid != nullptr);
    }
    function.Update(input, nodes, states, &overflow);
    function.Combine(s.merges.data(), static_cast<uint32_t>(s.merges.size()),
                     states, &overflow);

    // Written at the batch's rows, as every column of it is.
    ColumnBuffer buffer = s.context->TakeBuffer();
    ColumnChunk& chunk = buffer.chunk();
    int64_t* values = chunk.Values<int64_t>();
    chunk.validity.resize(kMaxBatchRows);
    if (selection.prefix()) {
      function.Finalize(states, nodes, count, values, &chunk.validity);
    } else {
      s.values.resize(count);
      s.valid.resize(count);
      function.Finalize(states, nodes, count, s.values.data(), &s.valid);
      for (uint32_t row = 0; row < count; ++row) {
        values[selection[row]] = s.values[row];
        chunk.validity.change(selection[row], s.valid.is_set(row));
      }
    }
    batch.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, values, &chunk.validity),
        std::move(buffer));
  }
  if (overflow) {
    s.status = base::ErrStatus("TREE ACCUMULATE: integer overflow");
    return false;
  }
  return true;
}

}  // namespace

TreeAccumulateUp::TreeAccumulateUp(TreeAccumulateSpec spec)
    : spec_(std::move(spec)), functions_(MakeFunctions(spec_)) {}
TreeAccumulateUp::~TreeAccumulateUp() = default;

TreeAccumulateDown::TreeAccumulateDown(TreeAccumulateSpec spec)
    : spec_(std::move(spec)), functions_(MakeFunctions(spec_)) {}
TreeAccumulateDown::~TreeAccumulateDown() = default;

std::unique_ptr<OperatorState> TreeAccumulateUp::MakeState(
    Context& context) const {
  return std::make_unique<AccumulateState>(context, functions_.size());
}
std::unique_ptr<OperatorState> TreeAccumulateDown::MakeState(
    Context& context) const {
  return std::make_unique<AccumulateState>(context, functions_.size());
}

base::Status TreeAccumulateUp::status(const OperatorState& state) const {
  return state.Cast<const AccumulateState>().status;
}
base::Status TreeAccumulateDown::status(const OperatorState& state) const {
  return state.Cast<const AccumulateState>().status;
}

bool TreeAccumulateUp::Process(RowBatch& batch, OperatorState& state) const {
  return Accumulate(spec_, functions_, /*up=*/true, batch,
                    state.Cast<AccumulateState>());
}

bool TreeAccumulateDown::Process(RowBatch& batch, OperatorState& state) const {
  return Accumulate(spec_, functions_, /*up=*/false, batch,
                    state.Cast<AccumulateState>());
}

}  // namespace perfetto::trace_processor::core::exec
