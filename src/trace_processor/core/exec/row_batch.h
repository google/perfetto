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
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/no_destructor.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// A batch of columns, all with the same rows, and which of those rows are
// kept.
//
// A batch has row_count() rows, and every column a window of as many. Its
// selection says which of them are kept: size() of them. Whoever reads a
// batch reads only the rows kept, the `i`th of which is batch row
// `selection()[i]`; Value() does both steps. Dropping rows narrows the
// selection and leaves the columns as they are.
//
// A column either borrows storage which outlives the run, such as a
// dataframe's, or is onto a ColumnBuffer the batch holds a copy of. Either
// way, keeping a batch's rows is copying the batch: its values are never
// copied, and stay as they are for as long as the copy is kept.
//
// The views themselves are either the batch's own or lent by its producer,
// which fills the same ones batch after batch (SetColumns): then a batch costs
// the producer only what changes in them. A batch copies lent views before
// changing any column, so whoever changes a batch never writes into its
// producer's.
class RowBatch {
 public:
  RowBatch() = default;
  RowBatch(const RowBatch&) = delete;
  RowBatch& operator=(const RowBatch&) = delete;
  RowBatch(RowBatch&& other) noexcept { *this = std::move(other); }
  RowBatch& operator=(RowBatch&& other) noexcept {
    own_columns_ = std::move(other.own_columns_);
    own_buffers_ = std::move(other.own_buffers_);
    if (other.owned_) {
      Repoint();
    } else {
      SetColumns(other.columns_, other.column_count_, other.buffers_,
                 other.buffer_count_);
    }
    selection_.CopyFrom(other.selection_);
    other.Reset();
    return *this;
  }

  // How many rows are kept.
  uint32_t size() const { return selection_.size(); }

  // How many rows each column has, kept or not.
  uint32_t row_count() const { return selection_.rows(); }

  // Gives every column `rows` rows, all kept.
  void SetRowCount(uint32_t rows) { selection_.Reset(rows); }

  const Selection& selection() const { return selection_; }
  Selection& mutable_selection() { return selection_; }

  // The value of `column` at the `row`th row kept, which must hold one.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T Value(uint32_t column, uint32_t row) const {
    return columns_[column].At<T>(selection_[row]);
  }

  // Whether `column` holds a value at the `row`th row kept.
  PERFETTO_ALWAYS_INLINE bool IsValid(uint32_t column, uint32_t row) const {
    return columns_[column].IsValid(selection_[row]);
  }

  uint32_t column_count() const { return column_count_; }
  const ColumnView& column(uint32_t column) const { return columns_[column]; }

  // The buffer `column` is onto, null if it borrows its storage.
  const ColumnBuffer& buffer(uint32_t column) const {
    static const base::NoDestructor<ColumnBuffer> kBorrowed;
    return column < buffer_count_ ? buffers_[column] : kBorrowed.ref();
  }

  // Points this batch at `other`'s columns and rows. Nothing is copied but
  // the views and selection: the buffers are shared.
  void CopyFrom(const RowBatch& other) {
    PERFETTO_DCHECK(&other != this);
    own_columns_.assign(other.columns_, other.columns_ + other.column_count_);
    own_buffers_.assign(other.buffers_, other.buffers_ + other.buffer_count_);
    Repoint();
    selection_.CopyFrom(other.selection_);
  }

  // Points this batch at `columns` of `other`, in that order, and its rows.
  void Project(const RowBatch& other, const std::vector<uint32_t>& columns) {
    Reset();
    for (uint32_t column : columns) {
      AddColumn(other.column(column), other.buffer(column));
    }
    selection_.CopyFrom(other.selection_);
  }

  // Adds `view`, which is onto `buffer`: one filled for this batch, or one
  // another batch is onto, null if that borrows its storage.
  void AddColumn(const ColumnView& view, ColumnBuffer buffer) {
    Own();
    own_columns_.push_back(view);
    if (buffer) {
      own_buffers_.resize(own_columns_.size());
      own_buffers_.back() = std::move(buffer);
    }
    Repoint();
  }
  // Adds `view`, onto storage which outlives the run, such as a dataframe's.
  // Storage refilled before then must be in a buffer instead.
  void AddBorrowedColumn(const ColumnView& view) {
    AddColumn(view, ColumnBuffer());
  }
  // Replaces every column with `views`, lent: each onto the buffer of the
  // same index in `buffers`, or borrowing its storage past the end of it.
  // Nothing is copied, so the caller keeps both as they are until it next
  // fills the batch. Keeps the rows.
  void SetColumns(const std::vector<ColumnView>& views,
                  const std::vector<ColumnBuffer>& buffers) {
    SetColumns(views.data(), static_cast<uint32_t>(views.size()),
               buffers.data(), static_cast<uint32_t>(buffers.size()));
  }
  // Replaces `column` with `view`, which is onto `buffer`.
  void SetColumn(uint32_t column, const ColumnView& view, ColumnBuffer buffer) {
    Own();
    own_columns_[column] = view;
    if (buffer || column < own_buffers_.size()) {
      if (column >= own_buffers_.size()) {
        own_buffers_.resize(column + 1);
      }
      own_buffers_[column] = std::move(buffer);
    }
    Repoint();
  }

  // Removes every column and row.
  void Reset() {
    own_columns_.clear();
    own_buffers_.clear();
    Repoint();
    selection_.Reset(0);
  }

 private:
  void SetColumns(const ColumnView* views,
                  uint32_t view_count,
                  const ColumnBuffer* buffers,
                  uint32_t buffer_count) {
    // Buffers kept from before would only be released later.
    own_buffers_.clear();
    owned_ = false;
    columns_ = views;
    column_count_ = view_count;
    buffers_ = buffers;
    buffer_count_ = buffer_count;
  }

  // Copies lent views, to change them.
  void Own() {
    if (!owned_) {
      own_columns_.assign(columns_, columns_ + column_count_);
      own_buffers_.assign(buffers_, buffers_ + buffer_count_);
      Repoint();
    }
  }

  // Points the columns at the batch's own views.
  void Repoint() {
    owned_ = true;
    columns_ = own_columns_.data();
    column_count_ = static_cast<uint32_t>(own_columns_.size());
    buffers_ = own_buffers_.data();
    buffer_count_ = static_cast<uint32_t>(own_buffers_.size());
  }

  // The columns, lent or the batch's own.
  const ColumnView* columns_ = nullptr;
  uint32_t column_count_ = 0;
  // By column, up to the last column onto a buffer: null for those borrowing
  // their storage.
  const ColumnBuffer* buffers_ = nullptr;
  uint32_t buffer_count_ = 0;
  bool owned_ = true;
  std::vector<ColumnView> own_columns_;
  std::vector<ColumnBuffer> own_buffers_;
  Selection selection_;
};

// Reads a flat column's values at the rows a batch keeps.
template <typename T>
class FlatColumnReader {
 public:
  FlatColumnReader(const RowBatch& batch, uint32_t column)
      : FlatColumnReader(batch.column(column), batch.selection()) {}
  FlatColumnReader(const ColumnView& column, const Selection& selection)
      : data_(static_cast<const T*>(column.data())),
        start_(column.start()),
        validity_(column.validity()),
        selection_(&selection) {
    PERFETTO_DCHECK(column.kind() == ColumnView::Kind::kFlat);
    PERFETTO_DCHECK(column.type().Is<typename TypeTagFor<T>::type>());
  }

  // False if the `row`th row kept holds no value.
  PERFETTO_ALWAYS_INLINE bool Read(uint32_t row, T* out) const {
    uint32_t index = start_ + (*selection_)[row];
    if (validity_ && !validity_->is_set(index)) {
      return false;
    }
    *out = data_[index];
    return true;
  }

 private:
  const T* data_;
  uint32_t start_;
  const BitVector* validity_;
  const Selection* selection_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
