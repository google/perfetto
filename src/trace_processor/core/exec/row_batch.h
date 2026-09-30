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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

// A batch of columns, all with the same number of rows.
//
// Published owned columns and composed selections are immutable while retained.
// Unowned values are borrowed; long-lived dataframe storage is published with
// its column owner. CopyFrom shares values and owns any borrowed selection
// indices. Owned views remain valid across producer advancement, rewind
// and destruction. Borrowed values expire at the producer's next call; a
// retaining consumer must materialize them.
class RowBatch {
 public:
  RowBatch() = default;
  RowBatch(const RowBatch&) = delete;
  RowBatch& operator=(const RowBatch&) = delete;
  RowBatch(RowBatch&&) noexcept = default;
  RowBatch& operator=(RowBatch&&) noexcept = default;

  uint32_t size() const { return cardinality_; }
  void SetCardinality(uint32_t count) {
    PERFETTO_DCHECK(count <= kMaxBatchRows);
    cardinality_ = count;
  }

  uint32_t column_count() const {
    return static_cast<uint32_t>(columns_.size());
  }
  const ColumnView& column(uint32_t column) const { return columns_[column]; }
  ColumnView& mutable_column(uint32_t column) { return columns_[column]; }

  const std::shared_ptr<const void>& owner(uint32_t column) const {
    static const std::shared_ptr<const void> kNone;
    return column < owners_.size() ? owners_[column] : kNone;
  }

  // Points this batch at `other`'s columns and cardinality. Nothing is copied:
  // values and indices are shared, and borrowed values remain borrowed.
  void CopyFrom(const RowBatch& other) {
    columns_ = other.columns_;
    if (!owners_.empty() || !other.owners_.empty()) {
      owners_ = other.owners_;
    }
    last_ = other.last_;
    changes_ |= kComposed | kResized;
    cardinality_ = other.cardinality_;
  }

  // Transfers views without copying their shared owners. Pools stay with the
  // batches which allocate from them so repeated execution reuses storage.
  void SwapContents(RowBatch& other) {
    std::swap(cardinality_, other.cardinality_);
    columns_.swap(other.columns_);
    owners_.swap(other.owners_);
    std::swap(last_, other.last_);
    changes_ |= kComposed | kResized;
    other.changes_ |= kComposed | kResized;
  }

  // Replaces `column` and the owner keeping its values alive.
  void SetColumn(uint32_t column,
                 ColumnView view,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_[column] = std::move(view);
    changes_ |= kReplaced;
    if (owner || column < owners_.size()) {
      owners_.resize(std::max<size_t>(owners_.size(), column + 1));
      owners_[column] = std::move(owner);
    }
  }
  // Adds a column. `owner` keeps the values alive for as long as the batch
  // does. A null owner declares borrowed storage; retaining consumers may copy.
  void AddColumn(ColumnView column,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_.push_back(std::move(column));
    changes_ |= kResized;
    if (owner) {
      owners_.resize(columns_.size());
      owners_.back() = std::move(owner);
    }
  }

  // Points every column at the `count` rows `selection` picks out.
  void Compose(RowSelection selection, uint32_t count);

  // Selects logical rows, permitting repetition and reordering. Returns false
  // when no rows remain. The values themselves are never copied.
  bool Slice(RowSelection selection, uint32_t count);

  // Removes every column.
  // What happened to the columns since this was last marked settled: what a
  // source restoring its own batch has to redo beyond pointing its columns at
  // new rows. One mask, so that source tests a single byte per run.
  enum Change : uint8_t {
    // A column was replaced, rather than added or narrowed.
    kReplaced = 1,
    // Columns may own the selections they point at.
    kComposed = 2,
    // Columns were added or removed.
    kResized = 4,
  };
  uint8_t changes() const { return changes_; }
  void mark_settled() { changes_ = 0; }

  // Whether this is known to be the last batch its source has.
  bool last() const { return last_; }
  void set_last(bool last) { last_ = last; }

  // Drops every column from `count` on.
  void TruncateColumns(uint32_t count) {
    if (count < columns_.size()) {
      columns_.resize(count);
      changes_ |= kResized;
    }
    if (count < owners_.size()) {
      owners_.resize(count);
    }
  }

  void Reset() {
    changes_ = kResized;
    last_ = false;
    cardinality_ = 0;
    columns_.clear();
    owners_.clear();
    selections_.Reset();
  }

 private:
  uint32_t cardinality_ = 0;
  bool last_ = false;
  // A new batch has no columns yet: it counts as resized.
  uint8_t changes_ = kResized;
  std::vector<ColumnView> columns_;
  // By column, up to the last column the batch owns; null for the others.
  std::vector<std::shared_ptr<const void>> owners_;
  SelectionPool selections_;
  // Reused scratch for composing each distinct mapping once, including when
  // columns sharing it are separated by a computed column.
  std::vector<std::pair<RowSelection, uint32_t>> compositions_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
