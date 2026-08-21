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

#include "src/trace_processor/core/exec/tree_order.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The column's values laid out flat; an index selection is gathered once.
const uint32_t* Flatten(const ColumnView& column,
                        uint32_t count,
                        std::vector<uint32_t>* scratch) {
  const auto* data = static_cast<const uint32_t*>(column.data());
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    return data + selection.offset();
  }
  scratch->resize(count);
  const uint32_t* rows = selection.data();
  uint32_t* out = scratch->data();
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = data[rows[i]];
  }
  return out;
}

const char* Name(TreeRowOrder order) {
  return order == TreeRowOrder::kParentFirst ? "TREE ORDER PARENT FIRST"
                                             : "TREE ORDER CHILD FIRST";
}

bool IsNodeColumn(const ColumnView& column) {
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Uint32>();
}

}  // namespace

TreeOrder::TreeOrder(const Source& input, Spec spec)
    : input_(input),
      spec_(spec),
      mode_(spec.input && *spec.input == spec.want ? Mode::kStreaming
                                                   : Mode::kBuffering) {}

TreeOrder::~TreeOrder() = default;
TreeOrder::State::~State() = default;

std::unique_ptr<OperatorState> TreeOrder::MakeState() const {
  auto state = std::make_unique<State>();
  state->input = input_.MakeState();
  return state;
}

base::Status TreeOrder::status(const OperatorState& state) const {
  const State& s = state.Cast<const State>();
  return s.status.ok() ? input_.status(*s.input) : s.status;
}

void TreeOrder::Note(State& s,
                     uint32_t node,
                     uint32_t parent,
                     uint32_t row) const {
  s.nodes_seen = std::max(s.nodes_seen, node + 1);
  if (parent != kNoNode) {
    s.nodes_seen = std::max(s.nodes_seen, parent + 1);
  }
  if (s.has_row.size() < s.nodes_seen) {
    s.has_row.resize(s.nodes_seen);
    s.row_of_node.resize(s.nodes_seen);
  }
  if (parent != kNoNode) {
    // A parent already seen means children follow parents, and the other way
    // round. A row cannot be both.
    if (s.has_row.is_set(parent)) {
      s.child_first = false;
    } else {
      s.parent_first = false;
    }
  }
  s.has_row.set(node);
  s.row_of_node[node] = row;
}

bool TreeOrder::Consume(RowBatch& batch, State& s, bool retain) const {
  uint32_t count = batch.size();
  if (count == 0) {
    return true;
  }
  const ColumnView& node_column = batch.column(spec_.node_column);
  const ColumnView& parent_column = batch.column(spec_.parent_column);
  if (!IsNodeColumn(node_column) || !IsNodeColumn(parent_column)) {
    s.status = base::ErrStatus(
        "%s: expected node numbers, which TREE NUMBER NODES makes",
        Name(spec_.want));
    return false;
  }
  const uint32_t* nodes = Flatten(node_column, count, &s.node_scratch);
  const uint32_t* parents = Flatten(parent_column, count, &s.parent_scratch);

  size_t base = s.nodes.size();
  s.nodes.insert(s.nodes.end(), nodes, nodes + count);
  s.parents.insert(s.parents.end(), parents, parents + count);
  for (uint32_t i = 0; i < count; ++i) {
    Note(s, nodes[i], parents[i], static_cast<uint32_t>(base) + i);
  }

  if (retain) {
    base::Status appended = s.rows.Append(batch);
    if (!appended.ok()) {
      s.status = appended;
      return false;
    }
  }
  return true;
}

bool TreeOrder::Sort(State& s) const {
  uint32_t nodes = s.nodes_seen;
  uint32_t rows = s.rows.size();
  // Children laid out end to end: a tree has as many entries as non-roots.
  std::vector<uint32_t> begin(nodes + 1, 0);
  for (uint32_t row = 0; row < rows; ++row) {
    if (s.parents[row] != kNoNode) {
      ++begin[s.parents[row] + 1];
    }
  }
  for (uint32_t node = 0; node < nodes; ++node) {
    begin[node + 1] += begin[node];
  }
  // Filling walks `begin` forward and puts it back, saving a second array.
  std::vector<uint32_t> children(begin[nodes]);
  for (uint32_t row = 0; row < rows; ++row) {
    if (s.parents[row] != kNoNode) {
      children[begin[s.parents[row]]++] = s.nodes[row];
    }
  }
  for (uint32_t node = nodes; node-- > 0;) {
    begin[node + 1] = begin[node];
  }
  begin[0] = 0;

  // A node is only reached through its parent, so this is parent-first.
  s.order.clear();
  s.order.reserve(rows);
  std::vector<uint32_t> pending;
  for (uint32_t row = 0; row < rows; ++row) {
    if (s.parents[row] == kNoNode) {
      pending.push_back(s.nodes[row]);
    }
  }
  while (!pending.empty()) {
    uint32_t node = pending.back();
    pending.pop_back();
    s.order.push_back(s.row_of_node[node]);
    for (uint32_t i = begin[node]; i < begin[node + 1]; ++i) {
      pending.push_back(children[i]);
    }
  }
  if (s.order.size() != rows) {
    s.status = base::ErrStatus(
        "%s: the rows are not a tree, because %u of them are in a cycle",
        Name(spec_.want), rows - static_cast<uint32_t>(s.order.size()));
    s.order.clear();
    return false;
  }
  return true;
}

bool TreeOrder::Fill(State& s) const {
  while (input_.GetData(s.input_batch, *s.input)) {
    if (!Consume(s.input_batch, s, /*retain=*/true)) {
      return false;
    }
  }
  if (!input_.status(*s.input).ok()) {
    s.status = input_.status(*s.input);
    return false;
  }
  if (s.has_row.size() < s.nodes_seen) {
    s.has_row.resize(s.nodes_seen);
  }
  if (s.has_row.CountSetBits() != s.nodes_seen) {
    s.status = base::ErrStatus(
        "%s: a row names a parent which is not itself a row in the input",
        Name(spec_.want));
    return false;
  }

  bool wanted =
      spec_.want == TreeRowOrder::kParentFirst ? s.parent_first : s.child_first;
  bool other =
      spec_.want == TreeRowOrder::kParentFirst ? s.child_first : s.parent_first;
  if (wanted) {
    // Straight through: no order needed.
    s.order.clear();
  } else if (other) {
    // Backwards is the whole of turning one tree order into the other.
    uint32_t rows = s.rows.size();
    s.order.resize(rows);
    for (uint32_t row = 0; row < rows; ++row) {
      s.order[row] = rows - 1 - row;
    }
  } else {
    if (!Sort(s)) {
      return false;
    }
    if (spec_.want == TreeRowOrder::kChildFirst) {
      std::reverse(s.order.begin(), s.order.end());
    }
  }
  s.filled = true;
  return true;
}

bool TreeOrder::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (!s.status.ok()) {
    return false;
  }
  if (mode_ == Mode::kStreaming) {
    if (!input_.GetData(out, *s.input)) {
      if (!input_.status(*s.input).ok()) {
        s.status = input_.status(*s.input);
        return false;
      }
      if (s.has_row.size() < s.nodes_seen) {
        s.has_row.resize(s.nodes_seen);
      }
      if (s.has_row.CountSetBits() != s.nodes_seen) {
        s.status = base::ErrStatus(
            "%s: a row names a parent which is not itself a row in the input",
            Name(spec_.want));
      }
      return false;
    }
    if (!Consume(out, s, /*retain=*/false)) {
      return false;
    }
    bool holds = spec_.want == TreeRowOrder::kParentFirst ? s.parent_first
                                                          : s.child_first;
    if (!holds) {
      s.status = base::ErrStatus(
          "%s: was told the rows already arrive in this order and they do not",
          Name(spec_.want));
      return false;
    }
    return true;
  }

  if (!s.filled && !Fill(s)) {
    return false;
  }
  if (s.emitted == s.rows.size()) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, s.rows.size() - s.emitted);
  if (s.order.empty()) {
    s.rows.View(&out, s.emitted, count);
  } else {
    const uint32_t* begin = s.order.data() + s.emitted;
    s.rows.View(&out, Span<const uint32_t>(begin, begin + count));
  }
  s.emitted += count;
  return true;
}

void TreeOrder::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  input_.Rewind(*s.input);
  s.emitted = 0;
  if (mode_ == Mode::kStreaming) {
    s.has_row.clear();
    s.nodes_seen = 0;
    s.parent_first = true;
    s.child_first = true;
    s.nodes.clear();
    s.parents.clear();
  }
  s.status = base::OkStatus();
}

TreeParentFirst::TreeParentFirst(const Source& input,
                                 uint32_t node_column,
                                 uint32_t parent_column,
                                 std::optional<TreeRowOrder> arriving)
    : TreeOrder(input,
                Spec{node_column, parent_column, TreeRowOrder::kParentFirst,
                     arriving}) {}

TreeParentFirst::~TreeParentFirst() = default;

TreeChildFirst::TreeChildFirst(const Source& input,
                               uint32_t node_column,
                               uint32_t parent_column,
                               std::optional<TreeRowOrder> arriving)
    : TreeOrder(input,
                Spec{node_column, parent_column, TreeRowOrder::kChildFirst,
                     arriving}) {}

TreeChildFirst::~TreeChildFirst() = default;

}  // namespace perfetto::trace_processor::core::exec
