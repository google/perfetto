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
// The values belong to whoever handed the batch over and stay valid until the
// next pull from them. A batch also keeps alive any column it was given
// ownership of, so such a column does not dangle if its producer goes away.
class RowBatch {
 public:
  RowBatch() = default;

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

  // Points this batch at `other`'s columns and cardinality. No values are
  // copied.
  void CopyFrom(const RowBatch& other) {
    columns_ = other.columns_;
    owners_ = other.owners_;
    cardinality_ = other.cardinality_;
    selections_.Reset();
    for (ColumnView& column : columns_) {
      column.DisownBlock();
    }
  }

  // Replaces `column` and the owner keeping its values alive.
  void SetColumn(uint32_t column,
                 ColumnView view,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_[column] = std::move(view);
    owners_[column] = std::move(owner);
  }
  // Adds a column. `owner` keeps the values alive for as long as the batch
  // does; pass null when the storage already outlives the batch.
  void AddColumn(ColumnView column,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_.push_back(std::move(column));
    owners_.push_back(std::move(owner));
  }

  // Points every column at `rows`, which the caller continues to own.
  bool AdoptPhysicalRows(Span<const uint32_t> rows);

  // Invalidates the current contents, ready for the batch to be refilled.
  void PrepareForFill() { selections_.Reset(); }

  // Points every column at the `count` rows `selection` picks out.
  void Compose(RowSelection selection, uint32_t count);

  // Narrows to the `count` rows `selection` picks out, whose ordinals must be
  // strictly increasing. Returns false when no rows remain.
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
