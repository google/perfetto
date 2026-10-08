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

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/no_destructor.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
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
// Published owned columns are immutable while retained. Unowned values are
// borrowed; long-lived dataframe storage is published with its column owner.
// Owned views remain valid across producer advancement, rewind and
// destruction. Borrowed values expire at the producer's next call; a
// retaining consumer must materialize them.
class RowBatch {
 public:
  RowBatch() = default;
  RowBatch(const RowBatch&) = delete;
  RowBatch& operator=(const RowBatch&) = delete;
  RowBatch(RowBatch&& other) noexcept { *this = std::move(other); }
  RowBatch& operator=(RowBatch&& other) noexcept {
    columns_ = std::move(other.columns_);
    owners_ = std::move(other.owners_);
    selection_.CopyFrom(other.selection_);
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

  uint32_t column_count() const {
    return static_cast<uint32_t>(columns_.size());
  }
  const ColumnView& column(uint32_t column) const { return columns_[column]; }
  ColumnView& mutable_column(uint32_t column) { return columns_[column]; }

  const std::shared_ptr<const void>& owner(uint32_t column) const {
    static const base::NoDestructor<std::shared_ptr<const void>> none;
    return column < owners_.size() ? owners_[column] : none.ref();
  }

  // Points this batch at `other`'s columns and rows. Nothing is copied but
  // the selection: values are shared, and borrowed values remain borrowed.
  void CopyFrom(const RowBatch& other) {
    columns_ = other.columns_;
    if (!owners_.empty() || !other.owners_.empty()) {
      owners_ = other.owners_;
    }
    selection_.CopyFrom(other.selection_);
  }

  // Points this batch at `columns` of `other`, in that order, and its rows.
  void Project(const RowBatch& other, const std::vector<uint32_t>& columns) {
    Reset();
    for (uint32_t column : columns) {
      AddColumn(other.column(column), other.owner(column));
    }
    selection_.CopyFrom(other.selection_);
  }

  // Replaces `column` and the owner keeping its values alive.
  void SetColumn(uint32_t column,
                 ColumnView view,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_[column] = view;
    if (owner || column < owners_.size()) {
      owners_.resize(std::max<size_t>(owners_.size(), column + 1));
      owners_[column] = std::move(owner);
    }
  }
  // Adds a column. `owner` keeps the values alive for as long as the batch
  // does. A null owner declares borrowed storage; retaining consumers may copy.
  void AddColumn(ColumnView column,
                 std::shared_ptr<const void> owner = nullptr) {
    columns_.push_back(column);
    if (owner) {
      owners_.resize(columns_.size());
      owners_.back() = std::move(owner);
    }
  }

  // Removes every column and row.
  void Reset() {
    columns_.clear();
    owners_.clear();
    selection_.Reset(0);
  }

 private:
  std::vector<ColumnView> columns_;
  // By column, up to the last column the batch owns; null for the others.
  std::vector<std::shared_ptr<const void>> owners_;
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
