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

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/batch_buffer.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// The rows a run holds on to, to hand back later: in runs, as they were held,
// or in any order, gathered. Rows are numbered from 0, in the order they were
// appended.
//
// Holding rows never costs much more than the rows themselves. A batch which
// keeps most of a batch's worth of rows, or whose columns all borrow storage
// which outlives the run, is held by sharing its buffers: nothing is copied.
// The rows any other batch keeps are packed, a full batch at a time, into
// buffers taken from the run's Context, so a few rows never keep whole
// buffers alive.
class RowStore {
 public:
  RowStore();
  ~RowStore();

  // Holds the rows `in` keeps. Every batch must have the same columns. Rows
  // packed are copied into buffers taken from `context`.
  base::Status Append(const RowBatch& in, Context& context);

  // How many rows are held.
  uint32_t size() const { return size_; }

  // The columns of the first batch appended, even one keeping no rows:
  // what each column holds. Valid once anything has been appended.
  const RowBatch& shape() const { return shape_; }

  // Points `out` at up to `count` rows from `offset` on, as they are held. A
  // run stops at the end of a batch held, so it can be shorter: returns how
  // many rows it has.
  uint32_t View(RowBatch* out, uint32_t offset, uint32_t count);

  // Fills `out` with up to kMaxBatchRows of `rows`, in that order and repeats
  // included. Returns how many. See GatherColumns().
  uint32_t Gather(RowBatch* out, Span<const uint32_t> rows, Context& context);

  // Adds to `out` the values of `columns` at `rows`, in that order and
  // repeats included: at most kMaxBatchRows of them, so that row i of each
  // column added is rows[i]. Rows held in one ascending run of one batch are
  // viewed where they are held; any others are copied into buffers taken from
  // `context`. The work done is in proportion to the rows, however many
  // batches are held.
  void GatherColumns(const std::vector<uint32_t>& columns,
                     Span<const uint32_t> rows,
                     Context& context,
                     RowBatch* out);

  // Lets go of every row held.
  void Clear();

 private:
  // Whether `in` is held by sharing its buffers rather than packed.
  static bool Shares(const RowBatch& in);

  // Holds `in` as it is, sharing its buffers.
  void Hold(const RowBatch& in);

  // Holds the rows packed so far as one batch.
  void Flush();

  // The batch held which row `row` is in.
  uint32_t Find(uint32_t row) const {
    PERFETTO_DCHECK(row < held_rows_);
    return batch_of_row_[row];
  }

  // The row `row` is of the batch it is in, before that batch's selection.
  uint32_t RowInBatch(uint32_t row, uint32_t batch) const {
    uint32_t start = batch ? ends_[batch - 1] : 0;
    return batches_[batch]->selection()[row - start];
  }

  // The first `held_` are the batches held, the rest kept for reuse.
  std::vector<std::unique_ptr<RowBatch>> batches_;
  uint32_t held_ = 0;
  // The rows in `batches_`: all but those still being packed.
  uint32_t held_rows_ = 0;
  // By batch held, the row after its last.
  FlexVector<uint32_t> ends_;
  // By row held, the batch it is in: found at once, as rows taken out of
  // order would otherwise each search the batches.
  FlexVector<uint32_t> batch_of_row_;
  // By column, where each batch held has its values: what gathering reads,
  // recorded as each batch is held so no gather has to look at batches its
  // rows are not in.
  std::vector<FlexVector<GatherSource>> sources_;
  // By column, whether any batch held has nulls in it.
  FlexVector<bool> nullable_;
  // The rows being packed, until there are a batch's worth or a batch is
  // shared after them.
  BatchBuffer packing_;
  RowBatch shape_;
  bool has_shape_ = false;
  // Every column, in order: what Gather() copies.
  std::vector<uint32_t> all_columns_;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
