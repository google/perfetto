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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

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
  T* dest = chunk.Values<T>() + offset;
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
  T* dest = chunk.Values<T>() + offset;
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

void ColumnChunk::Gather(const std::vector<const ColumnView*>& views,
                         const uint32_t* sources,
                         const uint32_t* rows,
                         uint32_t count,
                         std::vector<const void*>* bases) {
  bool nullable = false;
  bool sequence = false;
  for (const ColumnView* view : views) {
    nullable |= view->validity() != nullptr;
    sequence |= view->kind() == ColumnView::Kind::kSequence;
  }
  DispatchOnType(*views[0], [&](auto type) {
    using T = decltype(type);
    T* dest = Values<T>();
    if (sequence) {
      // A sequence has no storage to point at: rare, so read it as a view.
      for (uint32_t i = 0; i < count; ++i) {
        dest[i] = views[sources[i]]->At<T>(rows[i]);
      }
      return;
    }
    // Each view's values from its window's start, so a row is one load away.
    bases->resize(views.size());
    for (uint32_t v = 0; v < views.size(); ++v) {
      (*bases)[v] = static_cast<const T*>(views[v]->data()) + views[v]->start();
    }
    const void* const* base = bases->data();
    for (uint32_t i = 0; i < count; ++i) {
      dest[i] = static_cast<const T*>(base[sources[i]])[rows[i]];
    }
  });

  if (!nullable) {
    validity.resize(kMaxBatchRows);
    validity.FillBits(0, count, true);
    return;
  }
  // A word of bits at a time, rather than changing each in place.
  validity.clear();
  for (uint32_t begin = 0; begin < count; begin += 64) {
    uint32_t end = std::min(begin + 64, count);
    uint64_t word = 0;
    for (uint32_t i = begin; i < end; ++i) {
      const ColumnView& view = *views[sources[i]];
      const BitVector* source = view.validity();
      bool valid = !source || source->is_set(view.Index(rows[i]));
      word |= uint64_t{valid} << (i - begin);
    }
    validity.AppendWord(word);
  }
  validity.resize(kMaxBatchRows);
}

ColumnView ColumnChunk::View(const ColumnView& source, bool nullable) const {
  if (source.kind() == ColumnView::Kind::kVariant) {
    return ColumnView::Variants(Values<Variant>());
  }
  auto type = source.type().Is<Id>() ? StorageType{Uint32{}} : source.type();
  const void* data;
  if (type.Is<Uint32>()) {
    data = Values<uint32_t>();
  } else if (type.Is<Int32>()) {
    data = Values<int32_t>();
  } else if (type.Is<Int64>()) {
    data = Values<int64_t>();
  } else if (type.Is<Double>()) {
    data = Values<double>();
  } else {
    data = Values<StringPool::Id>();
  }
  return ColumnView::Reference(type, data, nullable ? &validity : nullptr);
}
}  // namespace perfetto::trace_processor::core::exec
