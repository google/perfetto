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
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

// A rectangular batch of columns with one shared cardinality.
//
// A batch owns nothing. Its columns are views over values belonging to the
// operator that produced it, which fixes both halves of the question:
//
//   Who owns these values?   Whatever handed you the batch.
//   How long are they good?  Until you ask it for the next batch, and never
//                            past its lifetime.
//
// So a batch is a borrow with the same terms everywhere, and an operator that
// needs rows for longer than that copies them into a RowStore of its own.
// Everything an operator hands out is read from something it holds, which is
// why a batch does not outlive it: nothing here is shared, reference counted,
// or kept alive behind the reader's back, and a reader that wants values to
// survive the operator that made them copies them, as it would from any other
// view.
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

  // Drops the columns, so the batch can be pointed at something else.
  void Reset() {
    cardinality_ = 0;
    columns_.clear();
    selections_.Reset();
  }

 private:
  uint32_t cardinality_ = 0;
  std::vector<ColumnView> columns_;
  SelectionPool selections_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
