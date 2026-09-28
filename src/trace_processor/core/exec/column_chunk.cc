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
#include <type_traits>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {
namespace {

template <typename T>
void CopyValues(const ColumnView& view,
                uint32_t count,
                uint32_t offset,
                ColumnChunk& chunk) {
  auto* dest = chunk.Values<T>().data() + offset;
  RowSelection selection = view.selection();
  if constexpr (std::is_arithmetic_v<T>) {
    // A sequence column has no storage: each value is the row it sits at.
    if (view.kind() == ColumnView::Kind::kSequence) {
      for (uint32_t i = 0; i < count; ++i) {
        dest[i] = static_cast<T>(selection.GetIndex(i));
      }
      return;
    }
  }
  selection.Gather(static_cast<const T*>(view.data()), count, dest);
}
}  // namespace

void ColumnChunk::CopyFrom(const ColumnView& view,
                           uint32_t count,
                           uint32_t offset) {
  if (view.kind() == ColumnView::Kind::kVariant) {
    CopyValues<Variant>(view, count, offset, *this);
  } else if (view.type().Is<Id>() || view.type().Is<Uint32>()) {
    CopyValues<uint32_t>(view, count, offset, *this);
  } else if (view.type().Is<Int32>()) {
    CopyValues<int32_t>(view, count, offset, *this);
  } else if (view.type().Is<Int64>()) {
    CopyValues<int64_t>(view, count, offset, *this);
  } else if (view.type().Is<Double>()) {
    CopyValues<double>(view, count, offset, *this);
  } else {
    CopyValues<StringPool::Id>(view, count, offset, *this);
  }
  validity.resize(kMaxBatchRows);
  for (uint32_t i = 0; i < count; ++i) {
    if (!view.validity() ||
        view.validity()->is_set(view.selection().GetIndex(i))) {
      validity.set(offset + i);
    } else {
      validity.clear(offset + i);
    }
  }
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
