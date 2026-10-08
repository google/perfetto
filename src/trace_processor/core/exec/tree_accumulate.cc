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
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// What one execution carries between batches: a running total per node, and
// the totals computed for the current batch.
class AccumulateState : public OperatorState {
 public:
  AccumulateState() : OperatorState(ResetEachRun{}) {}
  ~AccumulateState() override;
  void Reset() override {
    by_node.clear();
    status = base::OkStatus();
  }

  std::vector<int64_t> by_node;
  std::vector<uint32_t> node_scratch;
  std::vector<uint32_t> parent_scratch;
  std::vector<int64_t> value_scratch;
  BufferPool<FlexVector<int64_t>> buffers;
  std::shared_ptr<FlexVector<int64_t>> totals;
  base::Status status = base::OkStatus();
};

AccumulateState::~AccumulateState() = default;

// The column's values at the rows kept, laid out flat, gathering once unless
// the rows kept are the first ones.
template <typename T>
const T* Flatten(const ColumnView& column,
                 const Selection& selection,
                 std::vector<T>* scratch) {
  const auto* data = static_cast<const T*>(column.data()) + column.start();
  if (selection.prefix()) {
    return data;
  }
  scratch->resize(selection.size());
  for (uint32_t row = 0; row < selection.size(); ++row) {
    (*scratch)[row] = data[selection[row]];
  }
  return scratch->data();
}

const int64_t* FlattenValues(const ColumnView& column,
                             const Selection& selection,
                             std::vector<int64_t>* scratch) {
  const auto* data = static_cast<const int64_t*>(column.data());
  const BitVector* validity = column.validity();
  if (selection.prefix() && !validity) {
    return data + column.start();
  }
  scratch->resize(selection.size());
  for (uint32_t row = 0; row < selection.size(); ++row) {
    uint32_t index = column.Index(selection[row]);
    (*scratch)[row] = validity && !validity->is_set(index) ? 0 : data[index];
  }
  return scratch->data();
}

base::Status Validate(const RowBatch& in, TreeAccumulateSpec spec) {
  const ColumnView& node = in.column(spec.node_column);
  const ColumnView& parent = in.column(spec.parent_column);
  const ColumnView& value = in.column(spec.value_column);
  bool nodes_ok = node.kind() == ColumnView::Kind::kFlat &&
                  node.type().Is<Uint32>() && node.validity() == nullptr &&
                  parent.kind() == ColumnView::Kind::kFlat &&
                  parent.type().Is<Uint32>() && parent.validity() == nullptr;
  if (!nodes_ok) {
    return base::ErrStatus(
        "TREE ACCUMULATE: node columns must be non-null Uint32");
  }
  if (value.kind() != ColumnView::Kind::kFlat || !value.type().Is<Int64>()) {
    return base::ErrStatus("TREE ACCUMULATE: values must be Int64");
  }
  return base::OkStatus();
}

bool Add(AccumulateState& state, int64_t a, int64_t b, int64_t* out) {
  if (base::CheckedAdd(a, b, out)) {
    return true;
  }
  state.status = base::ErrStatus("TREE ACCUMULATE: integer overflow");
  return false;
}

void Grow(std::vector<int64_t>* by_node, uint32_t node) {
  if (by_node->size() <= node) {
    by_node->resize(node + 1, 0);
  }
}

// Appends a column of `totals` to `batch`.
void Emit(RowBatch& batch, const std::shared_ptr<FlexVector<int64_t>>& totals) {
  ColumnView column =
      ColumnView::Reference(StorageType{Int64{}}, totals->data());
  batch.AddColumn(column, totals);
}

}  // namespace

TreeAccumulateUp::TreeAccumulateUp(TreeAccumulateSpec spec) : spec_(spec) {}
TreeAccumulateUp::~TreeAccumulateUp() = default;

TreeAccumulateDown::TreeAccumulateDown(TreeAccumulateSpec spec) : spec_(spec) {}
TreeAccumulateDown::~TreeAccumulateDown() = default;

std::unique_ptr<OperatorState> TreeAccumulateUp::MakeState() const {
  return std::make_unique<AccumulateState>();
}
std::unique_ptr<OperatorState> TreeAccumulateDown::MakeState() const {
  return std::make_unique<AccumulateState>();
}

base::Status TreeAccumulateUp::status(const OperatorState& state) const {
  return state.Cast<const AccumulateState>().status;
}
base::Status TreeAccumulateDown::status(const OperatorState& state) const {
  return state.Cast<const AccumulateState>().status;
}

bool TreeAccumulateUp::Process(RowBatch& batch, OperatorState& state) const {
  AccumulateState& s = state.Cast<AccumulateState>();
  s.status = Validate(batch, spec_);
  if (!s.status.ok()) {
    return false;
  }
  uint32_t count = batch.size();
  const Selection& selection = batch.selection();
  const uint32_t* nodes = Flatten<uint32_t>(batch.column(spec_.node_column),
                                            selection, &s.node_scratch);
  const uint32_t* parents = Flatten<uint32_t>(batch.column(spec_.parent_column),
                                              selection, &s.parent_scratch);
  const int64_t* values = FlattenValues(batch.column(spec_.value_column),
                                        selection, &s.value_scratch);

  // Written at the batch's rows, as every column of it is.
  s.totals.reset();
  s.totals = s.buffers.Acquire();
  s.totals->resize(batch.row_count());
  int64_t* totals = s.totals->data();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t node = nodes[row];
    Grow(&s.by_node, node);
    // Every descendant has already been seen and added its value here, so the
    // total is final the moment the node arrives.
    int64_t total;
    if (!Add(s, values[row], s.by_node[node], &total)) {
      return false;
    }
    totals[selection[row]] = total;
    uint32_t parent = parents[row];
    if (parent != kNoNode) {
      Grow(&s.by_node, parent);
      if (!Add(s, s.by_node[parent], total, &s.by_node[parent])) {
        return false;
      }
    }
  }
  Emit(batch, s.totals);
  return true;
}

bool TreeAccumulateDown::Process(RowBatch& batch, OperatorState& state) const {
  AccumulateState& s = state.Cast<AccumulateState>();
  s.status = Validate(batch, spec_);
  if (!s.status.ok()) {
    return false;
  }
  uint32_t count = batch.size();
  const Selection& selection = batch.selection();
  const uint32_t* nodes = Flatten<uint32_t>(batch.column(spec_.node_column),
                                            selection, &s.node_scratch);
  const uint32_t* parents = Flatten<uint32_t>(batch.column(spec_.parent_column),
                                              selection, &s.parent_scratch);
  const int64_t* values = FlattenValues(batch.column(spec_.value_column),
                                        selection, &s.value_scratch);

  // Written at the batch's rows, as every column of it is.
  s.totals.reset();
  s.totals = s.buffers.Acquire();
  s.totals->resize(batch.row_count());
  int64_t* totals = s.totals->data();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t parent = parents[row];
    int64_t above = 0;
    if (parent != kNoNode) {
      Grow(&s.by_node, parent);
      above = s.by_node[parent];
    }
    int64_t total;
    if (!Add(s, values[row], above, &total)) {
      return false;
    }
    uint32_t node = nodes[row];
    Grow(&s.by_node, node);
    s.by_node[node] = total;
    totals[selection[row]] = total;
  }
  Emit(batch, s.totals);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
