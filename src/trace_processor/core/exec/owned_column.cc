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

#include "src/trace_processor/core/exec/owned_column.h"

#include <cstdint>
#include <cstring>
#include <variant>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

template <typename T, typename Variant>
FlexVector<T>& Ensure(Variant& values, uint32_t size) {
  if (!std::holds_alternative<FlexVector<T>>(values)) {
    values = FlexVector<T>();
  }
  auto& vec = std::get<FlexVector<T>>(values);
  if (vec.size() < size) {
    vec.resize(size);
  }
  return vec;
}

// Copies the `count` values `column` resolves to. A range resolves to a run,
// which copies whole; anything else is one gather, and neither walks a row
// view a value at a time.
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

void OwnedColumn::Fill(const ColumnView& column, uint32_t count) {
  Clear();
  Append(column, count);
}

void OwnedColumn::Append(const ColumnView& column, uint32_t count) {
  StorageType type = column.type();
  uint32_t at = size_;
  if (type.Is<Id>()) {
    CopyIds(column, at, count, values_);
    type = StorageType{Uint32{}};
  } else if (type.Is<Uint32>()) {
    Copy<uint32_t>(column, at, count, values_);
  } else if (type.Is<Int32>()) {
    Copy<int32_t>(column, at, count, values_);
  } else if (type.Is<Int64>()) {
    Copy<int64_t>(column, at, count, values_);
  } else if (type.Is<Double>()) {
    Copy<double>(column, at, count, values_);
  } else {
    Copy<StringPool::Id>(column, at, count, values_);
  }
  PERFETTO_DCHECK(at == 0 || type == type_);
  type_ = type;
  size_ = at + count;

  const BitVector* validity = column.validity();
  if (!validity && !nullable_) {
    return;
  }
  if (validity_.size() < size_) {
    validity_.resize(size_);
  }
  if (!nullable_) {
    // Everything already here arrived from a column with nothing missing.
    for (uint32_t i = 0; i < at; ++i) {
      validity_.set(i);
    }
    nullable_ = true;
  }
  if (!validity) {
    for (uint32_t i = 0; i < count; ++i) {
      validity_.set(at + i);
    }
    return;
  }
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    uint32_t offset = selection.offset();
    for (uint32_t i = 0; i < count; ++i) {
      validity_.change(at + i, validity->is_set(offset + i));
    }
    return;
  }
  const uint32_t* rows = selection.data();
  for (uint32_t i = 0; i < count; ++i) {
    validity_.change(at + i, validity->is_set(rows[i]));
  }
}

ColumnView OwnedColumn::View() const {
  const void* data = nullptr;
  if (type_.Is<Uint32>()) {
    data = std::get<FlexVector<uint32_t>>(values_).data();
  } else if (type_.Is<Int32>()) {
    data = std::get<FlexVector<int32_t>>(values_).data();
  } else if (type_.Is<Int64>()) {
    data = std::get<FlexVector<int64_t>>(values_).data();
  } else if (type_.Is<Double>()) {
    data = std::get<FlexVector<double>>(values_).data();
  } else {
    data = std::get<FlexVector<StringPool::Id>>(values_).data();
  }
  return ColumnView::Reference(type_, data, nullable_ ? &validity_ : nullptr);
}

}  // namespace perfetto::trace_processor::core::exec
