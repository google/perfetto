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

#include "src/trace_processor/core/exec/row_store.h"

#include <cstdint>
#include <cstring>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Grows a column to hold `size` values. Reserving first is the whole of it:
// resize allocates exactly what it is asked for, so a store filled a chunk at
// a time would otherwise reallocate and copy the column on every chunk, and
// filling it would cost a multiple of what the column is worth.
template <typename T, typename Variant>
FlexVector<T>& Ensure(Variant& values, uint32_t size) {
  if (!std::holds_alternative<FlexVector<T>>(values)) {
    values = FlexVector<T>();
  }
  auto& vec = std::get<FlexVector<T>>(values);
  if (vec.size() < size) {
    vec.reserve(size);
    vec.resize(size);
  }
  return vec;
}

// Copies the `count` values `column` resolves to. A range resolves to a run,
// which copies whole; anything else is one gather. Neither walks a row view a
// value at a time.
template <typename T, typename Variant>
void Copy(const ColumnView& column,
          uint32_t at,
          uint32_t count,
          Variant& values) {
  FlexVector<T>& out = Ensure<T>(values, at + count);
  const auto* data = static_cast<const T*>(column.data());
  RowSelection selection = column.selection();
  T* dest = out.data() + at;
  if (selection.is_range()) {
    memcpy(dest, data + selection.offset(), count * sizeof(T));
    return;
  }
  const uint32_t* rows = selection.data();
  for (uint32_t i = 0; i < count; ++i) {
    dest[i] = data[rows[i]];
  }
}

// An Id column's value is the row it sits at, so copying one means writing
// down the rows it stood for.
template <typename Variant>
void CopyIds(const ColumnView& column,
             uint32_t at,
             uint32_t count,
             Variant& values) {
  FlexVector<uint32_t>& out = Ensure<uint32_t>(values, at + count);
  RowSelection selection = column.selection();
  uint32_t* dest = out.data() + at;
  if (selection.is_range()) {
    uint32_t offset = selection.offset();
    for (uint32_t i = 0; i < count; ++i) {
      dest[i] = offset + i;
    }
    return;
  }
  memcpy(dest, selection.data(), count * sizeof(uint32_t));
}

}  // namespace

base::Status RowStore::AppendColumn(Column& into,
                                    const ColumnView& from,
                                    uint32_t count) {
  StorageType type = from.type();
  if (type.Is<Id>()) {
    CopyIds(from, size_, count, into.values);
    type = StorageType{Uint32{}};
  } else if (type.Is<Uint32>()) {
    Copy<uint32_t>(from, size_, count, into.values);
  } else if (type.Is<Int32>()) {
    Copy<int32_t>(from, size_, count, into.values);
  } else if (type.Is<Int64>()) {
    Copy<int64_t>(from, size_, count, into.values);
  } else if (type.Is<Double>()) {
    Copy<double>(from, size_, count, into.values);
  } else {
    Copy<StringPool::Id>(from, size_, count, into.values);
  }
  if (size_ != 0 && !(type == into.type)) {
    return base::ErrStatus(
        "row store: a column arrived holding one type after holding another");
  }
  into.type = type;

  const BitVector* validity = from.validity();
  if (!validity && !into.nullable) {
    return base::OkStatus();
  }
  uint32_t end = size_ + count;
  if (into.validity.size() < end) {
    into.validity.reserve(end);
    into.validity.resize(end);
  }
  if (!into.nullable) {
    // Everything already here came from a column with nothing missing.
    for (uint32_t i = 0; i < size_; ++i) {
      into.validity.set(i);
    }
    into.nullable = true;
  }
  if (!validity) {
    for (uint32_t i = 0; i < count; ++i) {
      into.validity.set(size_ + i);
    }
    return base::OkStatus();
  }
  RowSelection selection = from.selection();
  if (selection.is_range()) {
    uint32_t offset = selection.offset();
    for (uint32_t i = 0; i < count; ++i) {
      into.validity.change(size_ + i, validity->is_set(offset + i));
    }
    return base::OkStatus();
  }
  const uint32_t* rows = selection.data();
  for (uint32_t i = 0; i < count; ++i) {
    into.validity.change(size_ + i, validity->is_set(rows[i]));
  }
  return base::OkStatus();
}

base::Status RowStore::Append(const RowBatch& batch) {
  uint32_t count = batch.size();
  if (count == 0) {
    return base::OkStatus();
  }
  if (columns_.empty()) {
    columns_.resize(batch.column_count());
  } else if (columns_.size() != batch.column_count()) {
    return base::ErrStatus(
        "row store: a batch of %u columns arrived after one of %u",
        batch.column_count(), static_cast<uint32_t>(columns_.size()));
  }
  for (uint32_t col = 0; col < columns_.size(); ++col) {
    RETURN_IF_ERROR(AppendColumn(columns_[col], batch.column(col), count));
  }
  size_ += count;
  return base::OkStatus();
}

Span<const int64_t> RowStore::Int64Column(uint32_t column) const {
  const Column& held = columns_[column];
  PERFETTO_DCHECK(held.type.Is<Int64>());
  const FlexVector<int64_t>& values =
      std::get<FlexVector<int64_t>>(held.values);
  return Span<const int64_t>(values.data(), values.data() + size_);
}

ColumnView RowStore::ViewOf(const Column& column) const {
  const void* data = nullptr;
  if (column.type.Is<Uint32>()) {
    data = std::get<FlexVector<uint32_t>>(column.values).data();
  } else if (column.type.Is<Int32>()) {
    data = std::get<FlexVector<int32_t>>(column.values).data();
  } else if (column.type.Is<Int64>()) {
    data = std::get<FlexVector<int64_t>>(column.values).data();
  } else if (column.type.Is<Double>()) {
    data = std::get<FlexVector<double>>(column.values).data();
  } else {
    data = std::get<FlexVector<StringPool::Id>>(column.values).data();
  }
  return ColumnView::Reference(column.type, data,
                               column.nullable ? &column.validity : nullptr);
}

void RowStore::View(RowBatch* batch, uint32_t offset, uint32_t count) const {
  batch->Reset();
  for (const Column& column : columns_) {
    ColumnView view = ViewOf(column);
    view.SetRange(offset);
    batch->AddColumn(view);
  }
  batch->SetCardinality(count);
}

void RowStore::View(RowBatch* batch, Span<const uint32_t> rows) const {
  batch->Reset();
  for (const Column& column : columns_) {
    ColumnView view = ViewOf(column);
    view.SetBorrowedRows(rows);
    batch->AddColumn(view);
  }
  batch->SetCardinality(static_cast<uint32_t>(rows.size()));
}

void RowStore::Clear() {
  size_ = 0;
  for (Column& column : columns_) {
    column.nullable = false;
  }
}

}  // namespace perfetto::trace_processor::core::exec
