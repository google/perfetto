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

#include "src/trace_processor/core/exec/tree_number_nodes.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Reads a column of any width into one key per row. The type is dispatched on
// once per batch, so the loop itself has no per-row dispatch.
Variant AsKey(uint32_t v) {
  return Variant::Int64(v);
}
Variant AsKey(int32_t v) {
  return Variant::Int64(v);
}
Variant AsKey(int64_t v) {
  return Variant::Int64(v);
}
Variant AsKey(StringPool::Id v) {
  return Variant::String(v);
}

template <typename T>
void KeysOf(const ColumnView& column, uint32_t count, Variant* keys) {
  const auto* data = static_cast<const T*>(column.data());
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    const T* from = data + selection.offset();
    for (uint32_t i = 0; i < count; ++i) {
      keys[i] = AsKey(from[i]);
    }
    return;
  }
  const uint32_t* rows = selection.data();
  for (uint32_t i = 0; i < count; ++i) {
    keys[i] = AsKey(data[rows[i]]);
  }
}

void SequenceKeys(const ColumnView& column, uint32_t count, Variant* keys) {
  RowSelection selection = column.selection();
  for (uint32_t i = 0; i < count; ++i) {
    keys[i] = Variant::Int64(selection.GetIndex(i));
  }
}

// Reads a column of any type into one Variant per row. A null row is only
// allowed where `nullable`.
base::Status ReadKeys(const ColumnView& column,
                      uint32_t count,
                      bool nullable,
                      FlexVector<Variant>* out) {
  Variant* keys = out->data();
  if (column.kind() == ColumnView::Kind::kVariant) {
    const auto* cells = static_cast<const Variant*>(column.data());
    RowSelection selection = column.selection();
    for (uint32_t i = 0; i < count; ++i) {
      const Variant& cell = cells[selection.GetIndex(i)];
      if (cell.type == Variant::Type::kDouble) {
        return base::ErrStatus("TREE NUMBER NODES: an id cannot be a float");
      }
      if (cell.type == Variant::Type::kNull && !nullable) {
        return base::ErrStatus("TREE NUMBER NODES: a row has no id");
      }
      keys[i] = cell;
    }
    return base::OkStatus();
  }

  StorageType type = column.type();
  if (type.Is<Id>()) {
    SequenceKeys(column, count, keys);
  } else if (type.Is<Uint32>()) {
    KeysOf<uint32_t>(column, count, keys);
  } else if (type.Is<Int32>()) {
    KeysOf<int32_t>(column, count, keys);
  } else if (type.Is<Int64>()) {
    KeysOf<int64_t>(column, count, keys);
  } else if (type.Is<String>()) {
    KeysOf<StringPool::Id>(column, count, keys);
  } else {
    return base::ErrStatus("TREE NUMBER NODES: an id cannot be a float");
  }
  const BitVector* validity = column.validity();
  if (!validity) {
    return base::OkStatus();
  }
  // A column can carry a validity bitvector without any row being null, so
  // check whether a row is actually null rather than whether it could be.
  RowSelection selection = column.selection();
  for (uint32_t i = 0; i < count; ++i) {
    if (validity->is_set(selection.GetIndex(i))) {
      continue;
    }
    if (!nullable) {
      return base::ErrStatus("TREE NUMBER NODES: a row has no id");
    }
    keys[i] = Variant::Null();
  }
  return base::OkStatus();
}

// A table scanned in row order: the ids are the rows, so `start` onwards, and
// each parent refers to a row at or before its own. Copies the parents out,
// nulls as kNoNode, or gives up on the first row which is not like that.
bool ParentsInOrder(const ColumnView& ids,
                    const ColumnView& parents,
                    uint32_t count,
                    uint32_t start,
                    uint32_t* out) {
  if (ids.kind() == ColumnView::Kind::kVariant || !ids.type().Is<Id>() ||
      ids.validity() || !ids.selection().is_range() ||
      ids.selection().offset() != start ||
      parents.kind() == ColumnView::Kind::kVariant ||
      !parents.type().Is<Uint32>()) {
    return false;
  }
  const auto* data = static_cast<const uint32_t*>(parents.data());
  const BitVector* validity = parents.validity();
  RowSelection selection = parents.selection();
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t index = selection.GetIndex(i);
    if (validity && !validity->is_set(index)) {
      out[i] = kNoNode;
    } else if (data[index] <= start + i) {
      out[i] = data[index];
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

TreeNumberNodes::TreeNumberNodes(uint32_t id_column, uint32_t parent_column)
    : id_column_(id_column), parent_column_(parent_column) {}

TreeNumberNodes::~TreeNumberNodes() = default;
TreeNumberNodes::State::~State() = default;

std::unique_ptr<OperatorState> TreeNumberNodes::MakeState() const {
  auto state = std::make_unique<State>();
  state->ids = FlexVector<Variant>::CreateWithSize(kMaxBatchRows);
  state->parents = FlexVector<Variant>::CreateWithSize(kMaxBatchRows);
  state->nodes = FlexVector<uint32_t>::CreateWithSize(kMaxBatchRows);
  state->parent_nodes = FlexVector<uint32_t>::CreateWithSize(kMaxBatchRows);
  return state;
}

void TreeNumberNodes::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  s.dense = true;
  s.numbered = 0;
  s.numbers.Clear();
  s.has_row.clear();
  s.status = base::OkStatus();
}

base::Status TreeNumberNodes::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

bool TreeNumberNodes::IsDenseForTesting(const OperatorState& state) const {
  return state.Cast<const State>().dense;
}

uint32_t TreeNumberNodes::Number(State& s, const Variant& id) const {
  Key key = id.type == Variant::Type::kString
                ? Key{id.AsString().raw_id(), true}
                : Key{id.AsInt64(), false};
  if (s.dense) {
    if (!key.is_string) {
      // Every integer id below `numbered` was handed out in order.
      if (key.value >= 0 && static_cast<uint64_t>(key.value) < s.numbered) {
        return static_cast<uint32_t>(key.value);
      }
      if (key.value == static_cast<int64_t>(s.numbered)) {
        if (s.numbered == kNoNode) {
          s.status = base::ErrStatus(
              "TREE NUMBER NODES: the relation has too many nodes");
          return kNoNode;
        }
        return s.numbered++;
      }
    }
    // Not dense after all, so record the numbering identity had implied.
    s.dense = false;
    for (uint32_t n = 0; n < s.numbered; ++n) {
      s.numbers.Insert(Key{static_cast<int64_t>(n), false}, n);
    }
  }
  if (uint32_t* existing = s.numbers.Find(key); existing) {
    return *existing;
  }
  if (s.numbered == kNoNode) {
    s.status =
        base::ErrStatus("TREE NUMBER NODES: the relation has too many nodes");
    return kNoNode;
  }
  uint32_t assigned = s.numbered++;
  s.numbers.Insert(key, assigned);
  return assigned;
}

bool TreeNumberNodes::NumberInOrder(const RowBatch& in,
                                    uint32_t count,
                                    State& s) const {
  uint32_t start = s.numbered;
  if (count > kNoNode - start ||
      !ParentsInOrder(in.column(id_column_), in.column(parent_column_), count,
                      start, s.parent_nodes.data())) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    s.nodes[i] = start + i;
  }
  s.numbered = start + count;
  // A parent numbered ahead of its row must not be marked as seen.
  s.has_row.resize(start);
  s.has_row.resize(s.numbered, true);
  return true;
}

bool TreeNumberNodes::NumberByKey(const RowBatch& in,
                                  uint32_t count,
                                  State& s) const {
  base::Status status = ReadKeys(in.column(id_column_), count, false, &s.ids);
  if (status.ok()) {
    status = ReadKeys(in.column(parent_column_), count, true, &s.parents);
  }
  if (!status.ok()) {
    s.status = status;
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = Number(s, s.ids[i]);
    if (!s.status.ok()) {
      return false;
    }
    if (s.has_row.size() <= node) {
      // Grown geometrically, as resize allocates exactly what it is asked for.
      s.has_row.resize(std::max<uint64_t>(node + 1, s.has_row.size() * 2));
    }
    if (s.has_row.is_set(node)) {
      s.status = base::ErrStatus(
          "TREE NUMBER NODES: more than one row has the same id");
      return false;
    }
    s.has_row.set(node);
    s.nodes[i] = node;
    if (s.parents[i].type == Variant::Type::kNull) {
      s.parent_nodes[i] = kNoNode;
      continue;
    }
    s.parent_nodes[i] = Number(s, s.parents[i]);
    if (!s.status.ok()) {
      return false;
    }
  }
  return true;
}

OpResult TreeNumberNodes::Execute(const RowBatch& in,
                                  RowBatch& out,
                                  OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t count = in.size();
  bool in_order = s.dense && NumberInOrder(in, count, s);
  if (!in_order && !NumberByKey(in, count, s)) {
    return OpResult::kError;
  }
  out.CopyFrom(in);
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, s.nodes.data()));
  out.AddColumn(
      ColumnView::Reference(StorageType{Uint32{}}, s.parent_nodes.data()));
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
