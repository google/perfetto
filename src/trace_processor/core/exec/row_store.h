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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// The batches an operator holds on to, to hand their rows back later: in
// runs, as they were held, or in any order, gathered.
//
// Holding a batch copies only which of its rows it keeps: its columns stay
// onto the same buffers, shared. Rows are numbered from 0, in the order they
// were appended.
class RowStore {
 public:
  RowStore();
  ~RowStore();

  // Holds the rows `in` keeps. Every batch must have the same columns.
  base::Status Append(const RowBatch& in);

  // How many rows are held.
  uint32_t size() const { return size_; }

  // Points `out` at up to `count` rows from `offset` on, as they are held. A
  // run stops at the end of a batch appended, so it can be shorter: returns
  // how many rows it has.
  uint32_t View(RowBatch* out, uint32_t offset, uint32_t count) const;

  // Copies up to kMaxBatchRows of `rows`, in that order and repeats included,
  // into buffers taken from `context`. Returns how many. The work done is in
  // proportion to the rows copied, however many batches are held.
  uint32_t Gather(RowBatch* out, Span<const uint32_t> rows, Context& context);

  // Lets go of every row held.
  void Clear();

 private:
  // The batch held which row `row` is in.
  uint32_t Find(uint32_t row) const { return batch_of_row_[row]; }

  // The row `row` is of the batch it is in, before that batch's selection.
  uint32_t RowInBatch(uint32_t row, uint32_t batch) const {
    uint32_t start = batch ? ends_[batch - 1] : 0;
    return batches_[batch]->selection()[row - start];
  }

  // The first `held_` are the batches held, the rest kept for reuse.
  std::vector<std::unique_ptr<RowBatch>> batches_;
  uint32_t held_ = 0;
  // By batch held, the row after its last.
  FlexVector<uint32_t> ends_;
  // By row held, the batch it is in: found at once, as rows taken out of
  // order would otherwise each search the batches.
  FlexVector<uint32_t> batch_of_row_;
  // By column, where each batch held has its values: what gathering reads,
  // recorded as each batch is appended so no gather has to look at batches
  // its rows are not in.
  std::vector<FlexVector<GatherSource>> sources_;
  // By column, whether any batch held has nulls in it.
  FlexVector<bool> nullable_;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
