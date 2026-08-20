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

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Every parent precedes its children, so one pass backwards is enough.
// `node` is how a row says which node it is. It is a template rather than a
// pointer so the common case, where a row's position is its node number,
// carries no per-row branch.
template <typename Node>
void Fold(Node node,
          Span<const int64_t> parents,
          Span<const int64_t> values,
          uint32_t nodes,
          FlexVector<int64_t>* by_node) {
  auto rows = static_cast<uint32_t>(parents.size());
  by_node->resize(nodes);
  int64_t* totals = by_node->data();
  for (uint32_t i = 0; i < nodes; ++i) {
    totals[i] = 0;
  }
  for (uint32_t row = 0; row < rows; ++row) {
    totals[node(row)] = values[row];
  }
  for (uint32_t row = rows; row-- > 0;) {
    int64_t parent = parents[row];
    if (parent >= 0) {
      totals[static_cast<uint32_t>(parent)] += totals[node(row)];
    }
  }
}

}  // namespace

TreeAccumulateUp::TreeAccumulateUp(const Source& input,
                                   uint32_t parent_column,
                                   uint32_t value_column)
    : Breaker(input),
      parent_column_(parent_column),
      value_column_(value_column) {}

TreeAccumulateUp::~TreeAccumulateUp() = default;
TreeAccumulateUp::State::~State() = default;

std::unique_ptr<BreakerState> TreeAccumulateUp::MakeBreakerState() const {
  return std::make_unique<State>();
}

void TreeAccumulateUp::Rewind(BreakerState& state) const {
  State& s = state.Cast<State>();
  s.rows.Clear();
  s.emitted = 0;
}

base::Status TreeAccumulateUp::Consume(RowBatch& chunk,
                                       OperatorState& state) const {
  return state.Cast<State>().rows.Append(chunk);
}

base::Status TreeAccumulateUp::Finish(OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t rows = s.rows.size();
  if (rows == 0) {
    return base::OkStatus();
  }
  if (!s.rows.type(parent_column_).Is<Int64>() ||
      !s.rows.type(value_column_).Is<Int64>()) {
    return base::ErrStatus(
        "TREE ACCUMULATE UP: the parent and value columns must be integers");
  }
  Span<const int64_t> parents = s.rows.Int64Column(parent_column_);
  Span<const int64_t> values = s.rows.Int64Column(value_column_);

  Fold([](uint32_t row) { return row; }, parents, values, rows, s.totals.get());
  return base::OkStatus();
}

bool TreeAccumulateUp::Emit(RowBatch& out, BreakerState& state) const {
  State& s = state.Cast<State>();
  if (s.emitted == s.rows.size()) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, s.rows.size() - s.emitted);
  s.rows.View(&out, s.emitted, count);
  ColumnView totals =
      ColumnView::Reference(StorageType{Int64{}}, s.totals->data());
  totals.SetRange(s.emitted);
  out.AddColumn(totals, s.totals);
  s.emitted += count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
