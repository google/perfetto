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

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Reads a column of any width into one key per row. The type is settled once
// per batch, so the walk itself carries no dispatch.
int64_t AsKey(uint32_t v) {
  return v;
}
int64_t AsKey(int32_t v) {
  return v;
}
int64_t AsKey(int64_t v) {
  return v;
}
int64_t AsKey(StringPool::Id v) {
  return v.raw_id();
}

template <typename T>
void KeysOf(const ColumnView& column,
            uint32_t count,
            std::vector<int64_t>* out) {
  const auto* data = static_cast<const T*>(column.data());
  RowSelection selection = column.selection();
  out->resize(count);
  int64_t* keys = out->data();
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

void SequenceKeys(const ColumnView& column,
                  uint32_t count,
                  std::vector<int64_t>* out) {
  RowSelection selection = column.selection();
  out->resize(count);
  int64_t* keys = out->data();
  for (uint32_t i = 0; i < count; ++i) {
    keys[i] = selection.GetIndex(i);
  }
}

// Reads the keys and, where the column can hold nothing, which rows do.
base::Status ReadKeys(const ColumnView& column,
                      uint32_t count,
                      std::vector<int64_t>* keys,
                      std::vector<uint8_t>* nulls) {
  if (nulls) {
    nulls->assign(count, 0);
  }
  if (column.kind() == ColumnView::Kind::kVariant) {
    const auto* cells = static_cast<const Variant*>(column.data());
    RowSelection selection = column.selection();
    keys->resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const Variant& cell = cells[selection.GetIndex(i)];
      switch (cell.type) {
        case Variant::Type::kInt64:
          (*keys)[i] = cell.AsInt64();
          break;
        case Variant::Type::kString:
          (*keys)[i] = cell.AsString().raw_id();
          break;
        case Variant::Type::kDouble:
          return base::ErrStatus("TREE NUMBER NODES: an id cannot be a float");
        case Variant::Type::kNull:
          if (!nulls) {
            return base::ErrStatus("TREE NUMBER NODES: a row has no id");
          }
          (*nulls)[i] = 1;
          (*keys)[i] = 0;
          break;
      }
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
  // A column may carry a validity bitvector without any row using it, so what
  // matters is whether a row is actually missing rather than whether it could
  // be.
  RowSelection selection = column.selection();
  for (uint32_t i = 0; i < count; ++i) {
    bool null = !validity->is_set(selection.GetIndex(i));
    if (!null) {
      continue;
    }
    if (!nulls) {
      return base::ErrStatus("TREE NUMBER NODES: a row has no id");
    }
    (*nulls)[i] = 1;
  }
  return base::OkStatus();
}

}  // namespace

TreeNumberNodes::TreeNumberNodes(uint32_t id_column, uint32_t parent_column)
    : id_column_(id_column), parent_column_(parent_column) {}

TreeNumberNodes::~TreeNumberNodes() = default;
TreeNumberNodes::State::~State() = default;

std::unique_ptr<OperatorState> TreeNumberNodes::MakeState() const {
  return std::make_unique<State>();
}

void TreeNumberNodes::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  s.dense = true;
  s.numbered = 0;
  s.numbers.Clear();
  s.status = base::OkStatus();
}

base::Status TreeNumberNodes::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

uint32_t TreeNumberNodes::Number(State& s, int64_t key) const {
  if (s.dense) {
    // Everything below `numbered` was handed out in order.
    if (key >= 0 && static_cast<uint64_t>(key) < s.numbered) {
      return static_cast<uint32_t>(key);
    }
    if (key == static_cast<int64_t>(s.numbered)) {
      return s.numbered++;
    }
    // Not dense after all: write down what identity had been saying.
    s.dense = false;
    for (uint32_t n = 0; n < s.numbered; ++n) {
      s.numbers.Insert(static_cast<int64_t>(n), n);
    }
  }
  if (uint32_t* existing = s.numbers.Find(key); existing) {
    return *existing;
  }
  uint32_t assigned = s.numbered++;
  s.numbers.Insert(key, assigned);
  return assigned;
}

OpResult TreeNumberNodes::Execute(const RowBatch& in,
                                  RowBatch& out,
                                  OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t count = in.size();
  base::Status status =
      ReadKeys(in.column(id_column_), count, &s.id_keys, nullptr);
  if (status.ok()) {
    status = ReadKeys(in.column(parent_column_), count, &s.parent_keys,
                      &s.parent_null);
  }
  if (!status.ok()) {
    s.status = status;
    return OpResult::kError;
  }

  s.out->nodes.resize(count);
  s.out->parents.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    s.out->nodes[i] = Number(s, s.id_keys[i]);
    s.out->parents[i] =
        s.parent_null[i] ? kNoNode : Number(s, s.parent_keys[i]);
  }

  out.CopyFrom(in);
  out.AddColumn(
      ColumnView::Reference(StorageType{Uint32{}}, s.out->nodes.data()), s.out);
  out.AddColumn(
      ColumnView::Reference(StorageType{Uint32{}}, s.out->parents.data()),
      s.out);
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
