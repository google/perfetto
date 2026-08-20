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
#include "src/trace_processor/core/exec/owned_column.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

class RowBatchPool;

// A rectangular batch of columns with one shared cardinality.
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
  void AddColumn(ColumnView column) { columns_.push_back(std::move(column)); }

  // Copies `column`'s `count` values into storage this batch owns and adds a
  // column reading them, dense from row zero.
  //
  // Optional, and the caller's call: a view costs nothing to add and is right
  // whenever the storage behind it outlives the batch. This is for when it
  // does not, which is any source that materialises into buffers it refills,
  // and for an operator holding batches past the pull that produced them.
  void AddOwnedColumn(const ColumnView& column, uint32_t count);

  // Points every column at `rows`, which the caller continues to own. Only
  // valid while the columns still share the source's contiguous view, and
  // while `rows` stays valid until the batch is next filled.
  bool AdoptPhysicalRows(Span<const uint32_t> rows);

  // Returns the storage the batch composed its row views into. A source calls
  // this before refilling; from here the previous contents, and any indices
  // taken from them, are no longer valid.
  void PrepareForFill() { selections_.Reset(); }

  // Points every column at the `count` rows `selection` picks out. Sources use
  // this to fill a batch once they have restored the columns' starting views;
  // operators narrow a filled one with Slice.
  void Compose(RowSelection selection, uint32_t count);

  // Applies strictly increasing logical row ordinals to every column. Returns
  // false when no rows remain.
  bool Slice(RowSelection selection, uint32_t count);

 private:
  friend class RowBatchPool;

  void Reset() {
    cardinality_ = 0;
    columns_.clear();
    selections_.Reset();
    // The buffers stay: a pooled batch refills the ones it filled last time.
    owned_used_ = 0;
  }

  uint32_t cardinality_ = 0;
  std::vector<ColumnView> columns_;
  SelectionPool selections_;
  // Held by pointer so a column's view of one survives the vector growing.
  std::vector<std::unique_ptr<OwnedColumn>> owned_;
  uint32_t owned_used_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
