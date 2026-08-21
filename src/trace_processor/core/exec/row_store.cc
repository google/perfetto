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
#include <memory>
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
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using Chunk = RowStore::Chunk;
constexpr uint32_t kChunkRows = RowStore::kChunkRows;

// The chunk's values, made to hold kChunkRows of T the first time it is used.
template <typename T, typename V>
FlexVector<T>& Values(V& values) {
  // The default alternative is an empty vector, so holding the right type is
  // not enough to have room in it.
  if (!std::holds_alternative<FlexVector<T>>(values) ||
      std::get<FlexVector<T>>(values).size() < kChunkRows) {
    values = FlexVector<T>::CreateWithSize(kChunkRows);
  }
  return std::get<FlexVector<T>>(values);
}

// Copies `count` of `column`'s rows, starting at its row `from`, to `at`.
template <typename T, typename V>
void Copy(const ColumnView& column,
          uint32_t from,
          uint32_t at,
          uint32_t count,
          V& values) {
  T* dest = Values<T>(values).data() + at;
  const auto* data = static_cast<const T*>(column.data());
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    memcpy(dest, data + selection.offset() + from, count * sizeof(T));
    return;
  }
  const uint32_t* rows = selection.data() + from;
  for (uint32_t i = 0; i < count; ++i) {
    dest[i] = data[rows[i]];
  }
}

// An Id column has no storage: its value is the row it sits at.
template <typename V>
void CopyIds(const ColumnView& column,
             uint32_t from,
             uint32_t at,
             uint32_t count,
             V& values) {
  uint32_t* dest = Values<uint32_t>(values).data() + at;
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    uint32_t offset = selection.offset() + from;
    for (uint32_t i = 0; i < count; ++i) {
      dest[i] = offset + i;
    }
    return;
  }
  memcpy(dest, selection.data() + from, count * sizeof(uint32_t));
}

// Copies the validity of `count` of `column`'s rows, starting at its row
// `from`, to `at`. `at` onwards starts clear.
void CopyValidity(const ColumnView& column,
                  uint32_t from,
                  uint32_t at,
                  uint32_t count,
                  BitVector& into) {
  const BitVector* validity = column.validity();
  if (!validity) {
    for (uint32_t i = 0; i < count; ++i) {
      into.set(at + i);
    }
    return;
  }
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    into.SetBitsFrom(at, *validity, selection.offset() + from, count);
    return;
  }
  const uint32_t* rows = selection.data() + from;
  for (uint32_t i = 0; i < count; ++i) {
    if (validity->is_set(rows[i])) {
      into.set(at + i);
    }
  }
}

// Picks the rows `rows` names out of `chunks`, laying them out from zero. A
// gather cannot be a view because the rows come from more than one chunk.
template <typename T>
void Gather(const std::vector<std::shared_ptr<Chunk>>& chunks,
            Span<const uint32_t> rows,
            Chunk& into) {
  T* dest = Values<T>(into.values).data();
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const Chunk& chunk = *chunks[rows[i] / kChunkRows];
    dest[i] = std::get<FlexVector<T>>(chunk.values)[rows[i] % kChunkRows];
  }
}

void GatherValidity(const std::vector<std::shared_ptr<Chunk>>& chunks,
                    Span<const uint32_t> rows,
                    Chunk& into) {
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const Chunk& chunk = *chunks[rows[i] / kChunkRows];
    if (chunk.validity.is_set(rows[i] % kChunkRows)) {
      into.validity.set(i);
    }
  }
}

}  // namespace

RowStore::Chunk& RowStore::ChunkAt(Column& column, uint32_t index) const {
  if (column.chunks.size() <= index) {
    column.chunks.resize(index + 1);
  }
  if (!column.chunks[index]) {
    column.chunks[index] = std::make_shared<Chunk>();
  }
  return *column.chunks[index];
}

base::Status RowStore::AppendColumn(Column& into,
                                    const ColumnView& from,
                                    uint32_t count) {
  bool variant = from.kind() == ColumnView::Kind::kVariant;
  if (size_ != 0 && variant != into.variant) {
    return base::ErrStatus("row store: a column changed shape");
  }
  StorageType type = from.type();
  if (!variant) {
    if (type.Is<Id>()) {
      type = StorageType{Uint32{}};
    }
    if (size_ != 0 && !(type == into.type)) {
      return base::ErrStatus(
          "row store: a column arrived holding one type after holding another");
    }
  }
  bool nullable = into.nullable || from.validity() != nullptr;

  for (uint32_t done = 0; done < count;) {
    uint32_t row = size_ + done;
    uint32_t index = row / kChunkRows;
    uint32_t at = row % kChunkRows;
    uint32_t run = std::min(count - done, kChunkRows - at);
    Chunk& chunk = ChunkAt(into, index);
    if (variant) {
      Copy<Variant>(from, done, at, run, chunk.values);
    } else if (from.type().Is<Id>()) {
      CopyIds(from, done, at, run, chunk.values);
    } else if (type.Is<Uint32>()) {
      Copy<uint32_t>(from, done, at, run, chunk.values);
    } else if (type.Is<Int32>()) {
      Copy<int32_t>(from, done, at, run, chunk.values);
    } else if (type.Is<Int64>()) {
      Copy<int64_t>(from, done, at, run, chunk.values);
    } else if (type.Is<Double>()) {
      Copy<double>(from, done, at, run, chunk.values);
    } else {
      Copy<StringPool::Id>(from, done, at, run, chunk.values);
    }
    if (nullable) {
      // A chunk's validity is only made once the column is known to need it,
      // so rows already in this chunk have to be filled in as valid.
      if (chunk.validity.size() == 0) {
        chunk.validity = BitVector::CreateWithSize(kChunkRows);
        for (uint32_t i = 0; i < at; ++i) {
          chunk.validity.set(i);
        }
      }
      CopyValidity(from, done, at, run, chunk.validity);
    }
    done += run;
  }
  into.variant = variant;
  into.type = type;
  into.nullable = nullable;
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

ColumnView RowStore::ViewOf(const Column& column, const Chunk& chunk) const {
  if (column.variant) {
    return ColumnView::Variants(
        std::get<FlexVector<Variant>>(chunk.values).data());
  }
  const void* data = nullptr;
  if (column.type.Is<Uint32>()) {
    data = std::get<FlexVector<uint32_t>>(chunk.values).data();
  } else if (column.type.Is<Int32>()) {
    data = std::get<FlexVector<int32_t>>(chunk.values).data();
  } else if (column.type.Is<Int64>()) {
    data = std::get<FlexVector<int64_t>>(chunk.values).data();
  } else if (column.type.Is<Double>()) {
    data = std::get<FlexVector<double>>(chunk.values).data();
  } else {
    data = std::get<FlexVector<StringPool::Id>>(chunk.values).data();
  }
  return ColumnView::Reference(column.type, data,
                               column.nullable ? &chunk.validity : nullptr);
}

uint32_t RowStore::View(RowBatch* batch,
                        uint32_t offset,
                        uint32_t count) const {
  PERFETTO_DCHECK(offset + count <= size_);
  uint32_t at = offset % kChunkRows;
  uint32_t served = std::min(count, kChunkRows - at);
  batch->Reset();
  for (const Column& column : columns_) {
    const std::shared_ptr<Chunk>& chunk = column.chunks[offset / kChunkRows];
    ColumnView view = ViewOf(column, *chunk);
    view.SetRange(at);
    batch->AddColumn(view, chunk);
  }
  batch->SetCardinality(served);
  return served;
}

void RowStore::View(RowBatch* batch, Span<const uint32_t> rows) {
  auto count = static_cast<uint32_t>(rows.size());
  PERFETTO_DCHECK(count <= kChunkRows);
  batch->Reset();
  for (Column& column : columns_) {
    if (!column.gathered) {
      column.gathered = std::make_shared<Chunk>();
    }
    Chunk& into = *column.gathered;
    if (column.variant) {
      Gather<Variant>(column.chunks, rows, into);
    } else if (column.type.Is<Uint32>()) {
      Gather<uint32_t>(column.chunks, rows, into);
    } else if (column.type.Is<Int32>()) {
      Gather<int32_t>(column.chunks, rows, into);
    } else if (column.type.Is<Int64>()) {
      Gather<int64_t>(column.chunks, rows, into);
    } else if (column.type.Is<Double>()) {
      Gather<double>(column.chunks, rows, into);
    } else {
      Gather<StringPool::Id>(column.chunks, rows, into);
    }
    if (column.nullable) {
      if (into.validity.size() == 0) {
        into.validity = BitVector::CreateWithSize(kChunkRows);
      } else {
        into.validity.ClearAllBits();
      }
      GatherValidity(column.chunks, rows, into);
    }
    ColumnView view = ViewOf(column, into);
    view.SetRange(0);
    batch->AddColumn(view, column.gathered);
  }
  batch->SetCardinality(count);
}

void RowStore::Clear() {
  size_ = 0;
  for (Column& column : columns_) {
    column.nullable = false;
    for (const std::shared_ptr<Chunk>& chunk : column.chunks) {
      if (chunk) {
        chunk->validity.clear();
      }
    }
  }
}

}  // namespace perfetto::trace_processor::core::exec
