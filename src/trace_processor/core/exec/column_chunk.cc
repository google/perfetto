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

#include "src/trace_processor/core/exec/column_chunk.h"

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Copies the `count` values of `view` stored from `first` on into `chunk`
// from `offset`.
template <typename T>
void CopyRun(const ColumnView& view,
             uint32_t first,
             uint32_t count,
             uint32_t offset,
             ColumnChunk& chunk) {
  T* dest = chunk.Values<T>().data() + offset;
  if constexpr (std::is_arithmetic_v<T>) {
    // A sequence column has no storage: each value is where it is stored.
    if (view.kind() == ColumnView::Kind::kSequence) {
      for (uint32_t i = 0; i < count; ++i) {
        dest[i] = static_cast<T>(first + i);
      }
      return;
    }
  }
  memcpy(dest, static_cast<const T*>(view.data()) + first, count * sizeof(T));
}

// Copies the values of `view` stored at `index(i)`, for each i below `count`,
// into `chunk` from `offset`.
template <typename T, typename Index>
void CopyIndexed(const ColumnView& view,
                 Index index,
                 uint32_t count,
                 uint32_t offset,
                 ColumnChunk& chunk) {
  T* dest = chunk.Values<T>().data() + offset;
  if constexpr (std::is_arithmetic_v<T>) {
    if (view.kind() == ColumnView::Kind::kSequence) {
      for (uint32_t i = 0; i < count; ++i) {
        dest[i] = static_cast<T>(index(i));
      }
      return;
    }
  }
  const auto* data = static_cast<const T*>(view.data());
  for (uint32_t i = 0; i < count; ++i) {
    dest[i] = data[index(i)];
  }
}

// Calls `copy` with a value of the type `view` is copied as.
template <typename Copy>
void DispatchOnType(const ColumnView& view, Copy copy) {
  if (view.kind() == ColumnView::Kind::kVariant) {
    copy(Variant{});
  } else if (view.type().Is<Id>() || view.type().Is<Uint32>()) {
    copy(uint32_t{});
  } else if (view.type().Is<Int32>()) {
    copy(int32_t{});
  } else if (view.type().Is<Int64>()) {
    copy(int64_t{});
  } else if (view.type().Is<Double>()) {
    copy(double{});
  } else {
    copy(StringPool::Id{});
  }
}

// Copies the values of `view` stored at `index(i)`, for each i below `count`,
// and whether each is there, into `chunk` from `offset`.
template <typename Index>
void Copy(const ColumnView& view,
          Index index,
          uint32_t count,
          uint32_t offset,
          ColumnChunk& chunk) {
  DispatchOnType(view, [&](auto type) {
    CopyIndexed<decltype(type)>(view, index, count, offset, chunk);
  });
  chunk.validity.resize(kMaxBatchRows);
  const BitVector* source = view.validity();
  if (!source) {
    chunk.validity.FillBits(offset, count, true);
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    chunk.validity.change(offset + i, source->is_set(index(i)));
  }
}

}  // namespace

void ColumnChunk::CopyFrom(const RowBatch& batch,
                           uint32_t column,
                           uint32_t offset) {
  const ColumnView& view = batch.column(column);
  const Selection& selection = batch.selection();
  uint32_t start = view.start();
  uint32_t count = batch.size();
  if (!selection.prefix()) {
    Copy(
        view, [&](uint32_t i) { return start + selection[i]; }, count, offset,
        *this);
    return;
  }
  // The rows kept are the window's first: one run of values.
  DispatchOnType(view, [&](auto type) {
    CopyRun<decltype(type)>(view, start, count, offset, *this);
  });
  validity.resize(kMaxBatchRows);
  if (const BitVector* source = view.validity()) {
    validity.FillBits(offset, count, false);
    validity.SetBitsFrom(offset, *source, start, count);
  } else {
    validity.FillBits(offset, count, true);
  }
}

void ColumnChunk::Gather(const ColumnView& view,
                         Span<const uint32_t> rows,
                         uint32_t offset) {
  uint32_t start = view.start();
  const uint32_t* r = rows.data();
  Copy(
      view, [start, r](uint32_t i) { return start + r[i]; },
      static_cast<uint32_t>(rows.size()), offset, *this);
}

ColumnView ColumnChunk::View(const ColumnView& source, bool nullable) const {
  if (source.kind() == ColumnView::Kind::kVariant) {
    return ColumnView::Variants(Values<Variant>().data());
  }
  auto type = source.type().Is<Id>() ? StorageType{Uint32{}} : source.type();
  const void* data;
  if (type.Is<Uint32>()) {
    data = Values<uint32_t>().data();
  } else if (type.Is<Int32>()) {
    data = Values<int32_t>().data();
  } else if (type.Is<Int64>()) {
    data = Values<int64_t>().data();
  } else if (type.Is<Double>()) {
    data = Values<double>().data();
  } else {
    data = Values<StringPool::Id>().data();
  }
  return ColumnView::Reference(type, data, nullable ? &validity : nullptr);
}
}  // namespace perfetto::trace_processor::core::exec
