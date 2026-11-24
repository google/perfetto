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

#include "src/trace_processor/core/exec/dataframe_windows.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {

namespace {

bool IsSparse(const dataframe::NullStorage& nulls) {
  const auto& n = nulls.nullability();
  return n.Is<SparseNull>() || n.Is<SparseNullWithPopcountAlways>() ||
         n.Is<SparseNullWithPopcountUntilFinalization>();
}

// Lends `column` where it lies: a flat view onto its storage, or, for a
// column without a slot per row, a sparse view with its prefix popcount.
template <typename T>
ColumnView Lend(const dataframe::Column& column,
                StorageType type,
                const uint32_t* popcount) {
  const T* data =
      column.storage
          .template unchecked_data<typename core::TypeTagFor<T>::type>();
  const auto& nulls = column.null_storage;
  if (nulls.nullability().template Is<core::NonNull>()) {
    return ColumnView::Reference(type, data, nullptr);
  }
  const BitVector& bits = nulls.GetNullBitVector();
  if (nulls.nullability().template Is<core::DenseNull>()) {
    return ColumnView::Reference(type, data, &bits);
  }
  return ColumnView::Sparse(type, data, &bits, popcount);
}

}  // namespace

DataframeWindows::DataframeWindows() = default;
DataframeWindows::~DataframeWindows() = default;

const uint32_t* DataframeWindows::PopcountOf(const dataframe::Column& column) {
  const auto& nulls = column.null_storage;
  if (nulls.nullability().Is<SparseNullWithPopcountAlways>()) {
    return nulls.unchecked_get<SparseNull>()
        .prefix_popcount_for_cell_get.data();
  }
  popcounts_.push_back(nulls.GetNullBitVector().PrefixPopcount());
  return popcounts_.back().data();
}

void DataframeWindows::Init(
    const std::vector<std::shared_ptr<const dataframe::Column>>& columns) {
  PERFETTO_DCHECK(lent_.empty());
  lent_.resize(columns.size());
  buffers_.resize(columns.size());
  for (uint32_t i = 0; i < columns.size(); ++i) {
    const dataframe::Column& column = *columns[i];
    StorageType type = column.storage.type();
    const auto& nulls = column.null_storage;
    bool sparse = IsSparse(nulls);
    const uint32_t* popcount = sparse ? PopcountOf(column) : nullptr;
    if (type.Is<Id>()) {
      // A sparse Id column stores no values: each is its slot.
      lent_[i] = sparse
                     ? ColumnView::Sparse(StorageType{Uint32{}}, nullptr,
                                          &nulls.GetNullBitVector(), popcount)
                     : ColumnView::Reference(type, nullptr,
                                             nulls.MaybeGetNullBitVector());
    } else if (type.Is<Uint32>()) {
      lent_[i] = Lend<uint32_t>(column, type, popcount);
    } else if (type.Is<Int32>()) {
      lent_[i] = Lend<int32_t>(column, type, popcount);
    } else if (type.Is<Int64>()) {
      lent_[i] = Lend<int64_t>(column, type, popcount);
    } else if (type.Is<Double>()) {
      lent_[i] = Lend<double>(column, type, popcount);
    } else {
      lent_[i] = Lend<StringPool::Id>(column, type, popcount);
    }
  }
}

void DataframeWindows::FillKept(uint32_t from,
                                base::Span<const uint32_t> offsets,
                                RowBatch& out) {
  for (ColumnView& view : lent_) {
    view.set_start(from);
  }
  out.SetColumns(lent_, buffers_);
  out.SetRowCount(*(offsets.e - 1) + 1);
  out.mutable_selection().KeepRows(offsets);
}

void DataframeWindows::Fill(uint32_t from, uint32_t count, RowBatch& out) {
  // Only the windows move: the views stay as they were lent.
  for (ColumnView& view : lent_) {
    view.set_start(from);
  }
  out.SetColumns(lent_, buffers_);
  out.SetRowCount(count);
}

void RangeReader::Init(
    const std::vector<std::shared_ptr<const dataframe::Column>>& columns) {
  windows_.Init(columns);
  for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
    descending_[i] = kMaxBatchRows - 1 - i;
  }
}

bool RangeReader::ReadBackwards(RowBatch& out) {
  if (next_ == range_.b) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, next_ - range_.b);
  next_ -= count;
  windows_.Fill(next_, count, out);
  out.mutable_selection().Reorder(
      base::Span<const uint32_t>(descending_.data() + (kMaxBatchRows - count),
                                 descending_.data() + kMaxBatchRows));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
