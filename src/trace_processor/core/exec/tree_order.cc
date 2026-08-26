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

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr char kChildFirst[] = "TREE ORDER CHILD FIRST";

bool IsNodeColumn(const ColumnView& column) {
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Uint32>();
}

// Points `node` and `parent` at the batch's node number columns, or fails if
// that is not what they are.
base::Status NodeColumns(const RowBatch& batch,
                         uint32_t node_column,
                         uint32_t parent_column,
                         const char* name,
                         const ColumnView** node,
                         const ColumnView** parent) {
  *node = &batch.column(node_column);
  *parent = &batch.column(parent_column);
  if (!IsNodeColumn(**node) || !IsNodeColumn(**parent)) {
    return base::ErrStatus(
        "%s: expected node numbers, which TREE NUMBER NODES makes", name);
  }
  return base::OkStatus();
}

}  // namespace

TreeChildFirst::TreeChildFirst(const Source& input,
                               uint32_t node_column,
                               uint32_t parent_column)
    : Breaker(input),
      node_column_(node_column),
      parent_column_(parent_column) {}

TreeChildFirst::~TreeChildFirst() = default;
TreeChildFirst::State::~State() = default;

std::unique_ptr<Breaker::State> TreeChildFirst::CreateState() const {
  return std::make_unique<State>();
}

bool TreeChildFirst::Consume(const RowBatch& in, Breaker::State& state) const {
  State& s = state.Cast<State>();
  uint32_t count = in.size();
  if (count == 0) {
    return true;
  }
  const ColumnView* node_column;
  const ColumnView* parent_column;
  s.status = NodeColumns(in, node_column_, parent_column_, kChildFirst,
                         &node_column, &parent_column);
  if (!s.status.ok()) {
    return false;
  }

  auto base = static_cast<uint32_t>(s.nodes.size());
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = node_column->Value<uint32_t>(i);
    uint32_t parent = parent_column->Value<uint32_t>(i);
    s.nodes.push_back(node);
    s.parents.push_back(parent);
    if (node == parent) {
      s.status = base::ErrStatus("%s: a node is its own parent", kChildFirst);
      return false;
    }
    s.nodes_seen = std::max(s.nodes_seen, node + 1);
    if (parent != kNoNode) {
      s.nodes_seen = std::max(s.nodes_seen, parent + 1);
    }
    if (s.has_row.size() < s.nodes_seen) {
      // Grown geometrically, as resize allocates exactly what it is asked for.
      auto size = std::max<uint64_t>(s.nodes_seen, s.has_row.size() * 2);
      s.has_row.resize(size);
      s.row_of_node.resize(size);
    }
    if (s.has_row.is_set(node)) {
      s.status = base::ErrStatus("%s: more than one row has the same node",
                                 kChildFirst);
      return false;
    }
    if (parent != kNoNode) {
      // Having already seen the parent rules out child first; not having
      // seen it rules out parent first.
      if (s.has_row.is_set(parent)) {
        s.child_first = false;
      } else {
        s.parent_first = false;
      }
    }
    s.has_row.set(node);
    s.row_of_node[node] = base + i;
  }
  s.status = s.rows.Append(in);
  return s.status.ok();
}

bool TreeChildFirst::Sort(State& s) const {
  uint32_t nodes = s.nodes_seen;
  uint32_t rows = s.rows.size();
  // The children of every node, laid out end to end: a tree has as many
  // entries as it has non-roots.
  auto begin = FlexVector<uint32_t>::CreateFilled(nodes + 1, 0);
  for (uint32_t row = 0; row < rows; ++row) {
    if (s.parents[row] != kNoNode) {
      ++begin[s.parents[row] + 1];
    }
  }
  for (uint32_t node = 0; node < nodes; ++node) {
    begin[node + 1] += begin[node];
  }
  // Filling walks `begin` forward and then restores it, saving a second
  // array.
  auto children = FlexVector<uint32_t>::CreateWithSize(begin[nodes]);
  for (uint32_t row = 0; row < rows; ++row) {
    if (s.parents[row] != kNoNode) {
      children[begin[s.parents[row]]++] = s.nodes[row];
    }
  }
  for (uint32_t node = nodes; node-- > 0;) {
    begin[node + 1] = begin[node];
  }
  begin[0] = 0;

  // A node is only reached through its parent, so this comes out parent
  // first; reversing it makes the post-order wanted.
  s.order.clear();
  s.order.reserve(rows);
  FlexVector<uint32_t> pending;
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
        kChildFirst, rows - static_cast<uint32_t>(s.order.size()));
    s.order.clear();
    return false;
  }
  std::reverse(s.order.begin(), s.order.end());
  return true;
}

bool TreeChildFirst::Finish(Breaker::State& state) const {
  State& s = state.Cast<State>();
  if (s.has_row.CountSetBits() != s.nodes_seen) {
    s.status = base::ErrStatus(
        "%s: a row names a parent which is not itself a row in the input",
        kChildFirst);
    return false;
  }
  if (s.child_first) {
    // Already in the requested order, so no reordering is needed.
    s.order.clear();
    return true;
  }
  if (s.parent_first) {
    // Reversing is all it takes to turn one tree order into the other.
    uint32_t rows = s.rows.size();
    s.order.resize(rows);
    for (uint32_t row = 0; row < rows; ++row) {
      s.order[row] = rows - 1 - row;
    }
    return true;
  }
  return Sort(s);
}

bool TreeChildFirst::Serve(RowBatch& out, Breaker::State& state) const {
  State& s = state.Cast<State>();
  if (s.emitted == s.rows.size()) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, s.rows.size() - s.emitted);
  if (s.order.empty()) {
    // A run never spans two chunks, so the store says how much it served.
    count = s.rows.View(&out, s.emitted, count);
  } else {
    const uint32_t* begin = s.order.data() + s.emitted;
    s.rows.View(&out, Span<const uint32_t>(begin, begin + count));
  }
  s.emitted += count;
  return true;
}

void TreeChildFirst::Reset(Breaker::State& state) const {
  State& s = state.Cast<State>();
  s.has_row.clear();
  s.nodes_seen = 0;
  s.parent_first = true;
  s.child_first = true;
  s.nodes.clear();
  s.parents.clear();
  s.row_of_node.clear();
  s.rows.Clear();
  s.order.clear();
  s.emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
