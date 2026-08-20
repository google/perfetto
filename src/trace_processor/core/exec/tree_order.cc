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
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/owned_column.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The column's `count` values, laid out flat. A range already is flat; an
// index selection is gathered once rather than walked a value at a time.
const int64_t* Flatten(const ColumnView& column,
                       uint32_t count,
                       std::vector<int64_t>* scratch) {
  const auto* data = static_cast<const int64_t*>(column.data());
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    return data + selection.offset();
  }
  scratch->resize(count);
  const uint32_t* rows = selection.data();
  int64_t* out = scratch->data();
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = data[rows[i]];
  }
  return out;
}

// Which of the column's `count` values are null, laid out flat, or null when
// none of them are.
const uint8_t* FlattenNulls(const ColumnView& column,
                            uint32_t count,
                            std::vector<uint8_t>* scratch) {
  const BitVector* validity = column.validity();
  if (!validity) {
    return nullptr;
  }
  scratch->resize(count);
  uint8_t* out = scratch->data();
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    uint32_t offset = selection.offset();
    for (uint32_t i = 0; i < count; ++i) {
      out[i] = static_cast<uint8_t>(!validity->is_set(offset + i));
    }
    return out;
  }
  const uint32_t* rows = selection.data();
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>(!validity->is_set(rows[i]));
  }
  return out;
}

const char* Name(TreeRowOrder order) {
  return order == TreeRowOrder::kParentFirst ? "TREE ORDER PARENT FIRST"
                                             : "TREE ORDER CHILD FIRST";
}

}  // namespace

TreeOrder::TreeOrder(Source& input, Spec spec)
    : input_(input),
      spec_(spec),
      mode_(spec.input && *spec.input == spec.want ? Mode::kStreaming
                                                   : Mode::kBuffering) {}

TreeOrder::~TreeOrder() = default;

uint32_t TreeOrder::Number(int64_t id) {
  if (dense_) {
    // Everything below `numbered_` was handed out in order, so an id in that
    // range is already its own number.
    if (id >= 0 && static_cast<uint64_t>(id) < numbered_) {
      return static_cast<uint32_t>(id);
    }
    if (id == static_cast<int64_t>(numbered_)) {
      return numbered_++;
    }
    // Not dense after all. Write down what identity had been saying, then
    // carry on the slow way.
    dense_ = false;
    for (uint32_t n = 0; n < numbered_; ++n) {
      numbers_.Insert(static_cast<int64_t>(n), n);
    }
  }
  if (uint32_t* existing = numbers_.Find(id); existing) {
    return *existing;
  }
  uint32_t assigned = numbered_++;
  numbers_.Insert(id, assigned);
  return assigned;
}

void TreeOrder::Note(uint32_t node, int64_t parent) {
  if (has_row_.size() < numbered_) {
    has_row_.resize(numbered_);
  }
  if (parent >= 0) {
    // A parent already seen means children follow parents here; a parent not
    // seen yet means they precede them. A row cannot be both, so between them
    // the two orders are decided by the first row that is not a root.
    if (has_row_.is_set(static_cast<uint32_t>(parent))) {
      child_first_ = false;
    } else {
      parent_first_ = false;
    }
  }
  has_row_.set(node);
}

bool TreeOrder::Consume(RowBatch& batch, bool retain) {
  uint32_t count = batch.size();
  if (count == 0) {
    return true;
  }
  std::vector<int64_t> id_scratch;
  std::vector<int64_t> parent_scratch;
  std::vector<uint8_t> null_scratch;
  const ColumnView& parent_column = batch.column(spec_.parent_column);
  const int64_t* ids =
      Flatten(batch.column(spec_.id_column), count, &id_scratch);
  const int64_t* parents = Flatten(parent_column, count, &parent_scratch);
  const uint8_t* parent_null =
      FlattenNulls(parent_column, count, &null_scratch);

  size_t base = nodes_.size();
  nodes_.resize(base + count);
  parents_.resize(base + count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = Number(ids[i]);
    int64_t parent = -1;
    if (!parent_null || !parent_null[i]) {
      parent = static_cast<int64_t>(Number(parents[i]));
    }
    Note(node, parent);
    nodes_[base + i] = node;
    parents_[base + i] = parent;
  }

  if (retain) {
    if (store_.empty()) {
      for (uint32_t col = 0; col < batch.column_count(); ++col) {
        store_.push_back(std::make_unique<OwnedColumn>());
      }
    }
    for (uint32_t col = 0; col < batch.column_count(); ++col) {
      store_[col]->Append(batch.column(col), count);
    }
    total_ += count;
  }
  PERFETTO_DCHECK(batch.column_count() == spec_.input_columns);
  return true;
}

bool TreeOrder::Fill() {
  while (RowBatch* batch = input_.Next()) {
    if (!Consume(*batch, /*retain=*/true)) {
      return false;
    }
  }
  if (!input_.status().ok()) {
    status_ = input_.status();
    return false;
  }
  if (has_row_.size() < numbered_) {
    has_row_.resize(numbered_);
  }
  if (has_row_.CountSetBits() != numbered_) {
    status_ = base::ErrStatus(
        "%s: a row names a parent which is not itself a row in the input",
        Name(spec_.want));
    return false;
  }

  bool wanted =
      spec_.want == TreeRowOrder::kParentFirst ? parent_first_ : child_first_;
  bool other =
      spec_.want == TreeRowOrder::kParentFirst ? child_first_ : parent_first_;
  if (wanted) {
    // Straight through: no order to read the rows back in, so none is built.
    order_.clear();
  } else if (other) {
    // Backwards, which is the whole of turning a tree order into the other
    // one and costs an array of row numbers rather than moving any values.
    order_.resize(total_);
    for (uint32_t row = 0; row < total_; ++row) {
      order_[row] = total_ - 1 - row;
    }
  } else {
    status_ = base::ErrStatus(
        "%s: the rows arrive in neither tree order, which needs a sort",
        Name(spec_.want));
    return false;
  }

  for (const std::unique_ptr<OwnedColumn>& column : store_) {
    batch_.AddColumn(column->View());
  }
  batch_.AddColumn(ColumnView::Reference(StorageType{Int64{}}, nodes_.data()));
  batch_.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, parents_.data()));
  filled_ = true;
  return true;
}

RowBatch* TreeOrder::NextStreaming() {
  RowBatch* batch = input_.Next();
  if (!batch) {
    if (!input_.status().ok()) {
      status_ = input_.status();
      return nullptr;
    }
    if (has_row_.size() < numbered_) {
      has_row_.resize(numbered_);
    }
    if (has_row_.CountSetBits() != numbered_) {
      status_ = base::ErrStatus(
          "%s: a row names a parent which is not itself a row in the input",
          Name(spec_.want));
    }
    return nullptr;
  }
  uint32_t count = batch->size();
  size_t base = nodes_.size();
  if (!Consume(*batch, /*retain=*/false)) {
    return nullptr;
  }
  bool holds =
      spec_.want == TreeRowOrder::kParentFirst ? parent_first_ : child_first_;
  if (!holds) {
    status_ = base::ErrStatus(
        "%s: was told the rows already arrive in this order and they do not",
        Name(spec_.want));
    return nullptr;
  }

  // Nothing is held, so the values behind the two columns are this batch's
  // and are written over on the next pull.
  batch_nodes_.assign(nodes_.begin() + static_cast<long>(base), nodes_.end());
  batch_parents_.assign(parents_.begin() + static_cast<long>(base),
                        parents_.end());
  nodes_.resize(base);
  parents_.resize(base);
  batch->AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, batch_nodes_.data()));
  batch->AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, batch_parents_.data()));
  batch->SetCardinality(count);
  return batch;
}

RowBatch* TreeOrder::NextBuffered() {
  if (emitted_ == total_) {
    return nullptr;
  }
  uint32_t count = std::min(kMaxBatchRows, total_ - emitted_);
  for (uint32_t col = 0; col < batch_.column_count(); ++col) {
    ColumnView& column = batch_.mutable_column(col);
    if (order_.empty()) {
      column.SetRange(emitted_);
    } else {
      const uint32_t* begin = order_.data() + emitted_;
      column.SetBorrowedRows(Span<const uint32_t>(begin, begin + count));
    }
  }
  batch_.SetCardinality(count);
  emitted_ += count;
  return &batch_;
}

RowBatch* TreeOrder::Next() {
  if (!status_.ok()) {
    return nullptr;
  }
  if (mode_ == Mode::kStreaming) {
    return NextStreaming();
  }
  if (!filled_ && !Fill()) {
    return nullptr;
  }
  return NextBuffered();
}

void TreeOrder::Reset() {
  input_.Reset();
  emitted_ = 0;
  if (mode_ == Mode::kStreaming) {
    dense_ = true;
    numbered_ = 0;
    numbers_.Clear();
    has_row_.clear();
    parent_first_ = true;
    child_first_ = true;
    nodes_.clear();
    parents_.clear();
  }
  status_ = base::OkStatus();
}

TreeParentFirst::TreeParentFirst(Source& input,
                                 uint32_t id_column,
                                 uint32_t parent_column,
                                 uint32_t input_columns,
                                 std::optional<TreeRowOrder> arriving)
    : TreeOrder(input,
                Spec{id_column, parent_column, input_columns,
                     TreeRowOrder::kParentFirst, arriving}) {}

TreeParentFirst::~TreeParentFirst() = default;

TreeChildFirst::TreeChildFirst(Source& input,
                               uint32_t id_column,
                               uint32_t parent_column,
                               uint32_t input_columns,
                               std::optional<TreeRowOrder> arriving)
    : TreeOrder(input,
                Spec{id_column, parent_column, input_columns,
                     TreeRowOrder::kChildFirst, arriving}) {}

TreeChildFirst::~TreeChildFirst() = default;

}  // namespace perfetto::trace_processor::core::exec
