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

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The column's values laid out flat, gathering once if it has an index
// selection.
template <typename T>
const T* Flatten(const ColumnView& column,
                 uint32_t count,
                 std::vector<T>* scratch) {
  const auto* data = static_cast<const T*>(column.data());
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    return data + selection.offset();
  }
  scratch->resize(count);
  const uint32_t* rows = selection.data();
  T* out = scratch->data();
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = data[rows[i]];
  }
  return out;
}

void Grow(std::vector<int64_t>* by_node, uint32_t node) {
  if (by_node->size() <= node) {
    by_node->resize(node + 1, 0);
  }
}

// Fills `out` with the input columns plus a column of `totals`.
void Emit(const RowBatch& in,
          RowBatch& out,
          const std::shared_ptr<FlexVector<int64_t>>& totals) {
  out.CopyFrom(in);
  ColumnView column =
      ColumnView::Reference(StorageType{Int64{}}, totals->data());
  out.AddColumn(column, totals);
}

}  // namespace

AccumulateState::~AccumulateState() = default;

TreeAccumulateUp::TreeAccumulateUp(AccumulateSpec spec) : spec_(spec) {}
TreeAccumulateUp::~TreeAccumulateUp() = default;

TreeAccumulateDown::TreeAccumulateDown(AccumulateSpec spec) : spec_(spec) {}
TreeAccumulateDown::~TreeAccumulateDown() = default;

std::unique_ptr<OperatorState> TreeAccumulateUp::MakeState() const {
  return std::make_unique<AccumulateState>();
}
std::unique_ptr<OperatorState> TreeAccumulateDown::MakeState() const {
  return std::make_unique<AccumulateState>();
}

void TreeAccumulateUp::Rewind(OperatorState& state) const {
  state.Cast<AccumulateState>().by_node.clear();
}
void TreeAccumulateDown::Rewind(OperatorState& state) const {
  state.Cast<AccumulateState>().by_node.clear();
}

base::Status TreeAccumulateUp::status(const OperatorState& state) const {
  return state.Cast<const AccumulateState>().status;
}
base::Status TreeAccumulateDown::status(const OperatorState& state) const {
  return state.Cast<const AccumulateState>().status;
}

OpResult TreeAccumulateUp::Execute(const RowBatch& in,
                                   RowBatch& out,
                                   OperatorState& state) const {
  AccumulateState& s = state.Cast<AccumulateState>();
  uint32_t count = in.size();
  std::vector<uint32_t> node_scratch;
  std::vector<uint32_t> parent_scratch;
  std::vector<int64_t> value_scratch;
  const uint32_t* nodes =
      Flatten<uint32_t>(in.column(spec_.node_column), count, &node_scratch);
  const uint32_t* parents =
      Flatten<uint32_t>(in.column(spec_.parent_column), count, &parent_scratch);
  const int64_t* values =
      Flatten<int64_t>(in.column(spec_.value_column), count, &value_scratch);

  s.totals->resize(count);
  int64_t* totals = s.totals->data();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t node = nodes[row];
    Grow(&s.by_node, node);
    // Every descendant has already been seen and added its value here, so the
    // total is final the moment the node arrives.
    int64_t total = values[row] + s.by_node[node];
    totals[row] = total;
    uint32_t parent = parents[row];
    if (parent != kNoNode) {
      Grow(&s.by_node, parent);
      s.by_node[parent] += total;
    }
  }
  Emit(in, out, s.totals);
  return OpResult::kNeedMoreInput;
}

OpResult TreeAccumulateDown::Execute(const RowBatch& in,
                                     RowBatch& out,
                                     OperatorState& state) const {
  AccumulateState& s = state.Cast<AccumulateState>();
  uint32_t count = in.size();
  std::vector<uint32_t> node_scratch;
  std::vector<uint32_t> parent_scratch;
  std::vector<int64_t> value_scratch;
  const uint32_t* nodes =
      Flatten<uint32_t>(in.column(spec_.node_column), count, &node_scratch);
  const uint32_t* parents =
      Flatten<uint32_t>(in.column(spec_.parent_column), count, &parent_scratch);
  const int64_t* values =
      Flatten<int64_t>(in.column(spec_.value_column), count, &value_scratch);

  s.totals->resize(count);
  int64_t* totals = s.totals->data();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t parent = parents[row];
    int64_t above = 0;
    if (parent != kNoNode) {
      Grow(&s.by_node, parent);
      above = s.by_node[parent];
    }
    int64_t total = values[row] + above;
    uint32_t node = nodes[row];
    Grow(&s.by_node, node);
    s.by_node[node] = total;
    totals[row] = total;
  }
  Emit(in, out, s.totals);
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
