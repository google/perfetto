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
    return owners_[column];
  }

  // Points this batch at `other`'s columns and cardinality. Nothing is copied:
  // values and indices are shared, and borrowed values remain borrowed.
  void CopyFrom(const RowBatch& other) {
    columns_ = other.columns_;
    owners_ = other.owners_;
    cardinality_ = other.cardinality_;
  }

  // Transfers views without copying their shared owners. Pools stay with the
  // batches which allocate from them so repeated execution reuses storage.
  void SwapContents(RowBatch& other) {
    std::swap(cardinality_, other.cardinality_);
    columns_.swap(other.columns_);
    owners_.swap(other.owners_);
  }

  // Replaces `column` and the owner keeping its values alive.
  void SetColumn(uint32_t column,
                 ColumnView view,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_[column] = std::move(view);
    owners_[column] = std::move(owner);
  }
  // Adds a column. `owner` keeps the values alive for as long as the batch
  // does. A null owner declares borrowed storage; retaining consumers may copy.
  void AddColumn(ColumnView column,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_.push_back(std::move(column));
    owners_.push_back(std::move(owner));
  }

  // Points every column at the `count` rows `selection` picks out.
  void Compose(RowSelection selection, uint32_t count);

  // Selects logical rows, permitting repetition and reordering. Returns false
  // when no rows remain. The values themselves are never copied.
  bool Slice(RowSelection selection, uint32_t count);

  // Removes every column.
  void Reset() {
    cardinality_ = 0;
    columns_.clear();
    owners_.clear();
    selections_.Reset();
  }

 private:
  uint32_t cardinality_ = 0;
  std::vector<ColumnView> columns_;
  // One per column; null for columns the batch does not own.
  std::vector<std::shared_ptr<const void>> owners_;
  SelectionPool selections_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
