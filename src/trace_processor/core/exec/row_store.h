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
#include <limits>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/batch_buffer.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_vector.h"

namespace perfetto::trace_processor::core::exec {

// Rows read where a RowStore holds them, in an order of its rows (see
// RowStore::Place). Plain data, held for one run of the state owning it.
struct RowsInPlace {
  // Each batch held, with every column: those not held read as nulls.
  RunVector<const RowBatch*> batches;
  // By position, which of `batches` the row is in, and its batch row there.
  RunVector<uint32_t> batch;
  RunVector<uint32_t> row;
};

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
//
// Plain data, held for one run of the state `owner` which contains it. Every
// call which can hold more rows takes the owner (see RunVector).
class RowStore {
 public:
  // Holds the rows `in` keeps. Every batch must have the same columns. Rows
  // packed are copied into buffers taken from the Context.
  base::Status Append(const RowBatch& in, OperatorState& owner);

  // How many rows are held.
  uint32_t size() const { return size_; }
  // The first batch appended, for its columns' types, or null if there was
  // none.
  const RowBatch* shape() const { return has_shape_ ? shape_ : nullptr; }

  // Points `out` at up to `count` rows from `offset` on, as they are held. A
  // run stops at the end of a batch held, so it can be shorter: returns how
  // many rows it has.
  uint32_t View(RowBatch* out,
                uint32_t offset,
                uint32_t count,
                OperatorState& owner);

  // Fills `out` with up to kMaxBatchRows of `rows`, in that order and repeats
  // included. Returns how many. Rows held in one ascending run of one batch
  // are viewed where they are held; any others are copied into buffers taken
  // from the Context. The work done is in proportion to the rows, however
  // many batches are held.
  uint32_t Gather(RowBatch* out,
                  base::Span<const uint32_t> rows,
                  OperatorState& owner) {
    return Gather(out, rows, nullptr, owner);
  }

  // Same as Gather(), but if `kept` isn't null, only the rows it selects are
  // read and written. The other output rows are left unspecified.
  uint32_t Gather(RowBatch* out,
                  base::Span<const uint32_t> rows,
                  const Selection* kept,
                  OperatorState& owner);

  // As Gather(), but rows held in one batch, in any order, are viewed where
  // they are held, with a selection putting them in order. `rows` must not
  // repeat, and the caller must not add a column laid out by position.
  uint32_t GatherInPlace(RowBatch* out,
                         base::Span<const uint32_t> rows,
                         OperatorState& owner);

  // Points `out` at `rows`, in that order, where they are held, so they are
  // read without being copied. Fails if any rows were packed, as packed
  // columns can differ in kind from those of the batches shared.
  bool Place(base::Span<const uint32_t> rows,
             RowsInPlace* out,
             OperatorState& owner);

  // Lets go of every row held.
  void Clear();

  // By column of the batches appended, whether it is never read back: such
  // a column is not held, and reads back as nulls of its type. Variant
  // columns are always held. Applies from the next Append() after a Clear().
  // `unread` must outlive the store.
  void SetUnread(base::Span<const bool> unread) { unread_ = unread; }

 private:
  static constexpr uint32_t kNotHeld = std::numeric_limits<uint32_t>::max();

  // Decides, from the first batch appended, which of its columns are held.
  void SetShape(const RowBatch& in, OperatorState& owner);

  // Whether a column is not held.
  bool drops() const { return held_columns_.size() != shape_->column_count(); }

  // Holds the rows `in` keeps, of the columns held.
  void Keep(const RowBatch& in, OperatorState& owner);

  // Whether `in` is held by sharing its buffers rather than packed.
  static bool Shares(const RowBatch& in);

  // Holds `in` as it is, sharing its buffers.
  void Hold(const RowBatch& in, OperatorState& owner);

  // Holds the rows packed so far as one batch.
  void Flush(OperatorState& owner);

  // Points `out` at `held`, with every column of the shape.
  void ViewHeld(const RowBatch& held, RowBatch* out) const;

  // Where row `row` is stored (see kPlaceRowBits). The row within its batch
  // is a physical row, before the batch's selection is applied. Rows in the
  // dense run (see dense_) are their own place.
  uint32_t PlaceOf(uint32_t row) const {
    PERFETTO_DCHECK(row < held_rows_);
    return row < dense_ ? row : places_[row - dense_];
  }

  // The index of the batch holding row `row`.
  uint32_t Find(uint32_t row) const { return PlaceOf(row) >> kPlaceRowBits; }

  // If `rows` are consecutive rows of one batch, points `out` at them without
  // copying and returns true.
  bool ViewIfRun(RowBatch* out, const uint32_t* rows, uint32_t count);

  // `count` consecutive rows from `first`, all in `batch`, viewed in place.
  uint32_t ViewRun(RowBatch* out,
                   uint32_t batch,
                   uint32_t first,
                   uint32_t count) const;

  // The row `row` is of the batch it is in, before that batch's selection.
  uint32_t RowInBatch(uint32_t row) const {
    return PlaceOf(row) & kPlaceRowMask;
  }

  // The batches taken from the Context for this run. The first `held_` hold
  // rows; the rest are kept for reuse.
  RunVector<RowBatch*> batches_;
  uint32_t held_ = 0;
  // The rows in `batches_`: all but those still being packed.
  uint32_t held_rows_ = 0;
  // By batch held, the row after its last.
  RunVector<uint32_t> ends_;
  // Rows are stored in batches. While every batch is full and unfiltered, row
  // r is simply row r % kMaxBatchRows of batch r / kMaxBatchRows, so its
  // place equals r. `dense_` counts the rows from the start where that holds.
  // For the rows after them, `places_` records each row's place as it's
  // appended, so a lookup never has to search the batches.
  uint32_t dense_ = 0;
  RunVector<uint32_t> places_;
  // By column, where each batch held has its values: what gathering reads,
  // recorded as each batch is held so no gather has to look at batches its
  // rows are not in.
  RunVector<RunVector<GatherSource>, RunKeep::kAll> sources_;
  // By column, whether any stored batch has nulls in it, and whether any has
  // it as a sequence (an id column with no stored values).
  RunVector<bool> nullable_;
  RunVector<bool> sequence_;
  // The rows being packed, until there are a batch's worth or a batch is
  // shared after them.
  BatchBuffer packing_;
  // What each column holds, taken from the first batch appended, even if that
  // batch keeps no rows. A column which is not held reads back as nulls. Null
  // until the first batch is appended.
  RowBatch* shape_ = nullptr;
  bool has_shape_ = false;
  base::Span<const bool> unread_;
  // The columns held, and by column, where it is among them or kNotHeld.
  RunVector<uint32_t> held_columns_;
  RunVector<uint32_t> stored_column_;
  // Whether any rows were packed.
  bool packed_ = false;
  // Whether a column held is sparse, which only Place() can read.
  bool holds_sparse_ = false;
  // When columns are not held: each batch held, viewed with every column.
  RunVector<RowBatch*> views_;
  // Scratch: the columns held of a batch appended.
  RowBatch* trimmed_ = nullptr;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
