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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kChunkRows = RowStore::kChunkRows;

// Copies `count` of `column`'s rows, starting at its row `from`, to `at`.
template <typename T>
void Copy(const ColumnView& column,
          uint32_t from,
          uint32_t at,
          uint32_t count,
          ColumnChunk& chunk) {
  T* dest = chunk.Values<T>().data() + at;
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
void CopyIds(const ColumnView& column,
             uint32_t from,
             uint32_t at,
             uint32_t count,
             ColumnChunk& chunk) {
  uint32_t* dest = chunk.Values<uint32_t>().data() + at;
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
// `from`, to `at`.
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
    } else {
      into.clear(at + i);
    }
  }
}

// Picks the rows `rows` names out of `chunks`, laying them out from zero. A
// gather cannot be a view because the rows come from more than one chunk.
template <typename T>
void Gather(const std::vector<std::unique_ptr<ColumnChunk>>& chunks,
            Span<const uint32_t> rows,
            ColumnChunk& into) {
  T* dest = into.Values<T>().data();
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const ColumnChunk& chunk = *chunks[rows[i] / kChunkRows];
    dest[i] = chunk.Values<T>()[rows[i] % kChunkRows];
  }
}

void GatherValidity(const std::vector<std::unique_ptr<ColumnChunk>>& chunks,
                    Span<const uint32_t> rows,
                    ColumnChunk& into) {
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const ColumnChunk& chunk = *chunks[rows[i] / kChunkRows];
    if (chunk.validity.is_set(rows[i] % kChunkRows)) {
      into.validity.set(i);
    }
  }
}

}  // namespace

RowStore::RowStore() = default;
RowStore::~RowStore() = default;

ColumnChunk& RowStore::ChunkAt(Column& column, uint32_t index) const {
  if (column.chunks.size() <= index) {
    column.chunks.resize(index + 1);
  }
  if (!column.chunks[index]) {
    column.chunks[index] = std::make_unique<ColumnChunk>();
  }
  return *column.chunks[index];
}

base::Status RowStore::ValidateColumn(const Column& into,
                                      const ColumnView& from) const {
  if (!initialized_) {
    return base::OkStatus();
  }
  bool variant = from.kind() == ColumnView::Kind::kVariant;
  if (variant != into.variant) {
    return base::ErrStatus("row store: a column changed shape");
  }
  if (variant) {
    return base::OkStatus();
  }
  StorageType type = from.type().Is<Id>() ? StorageType{Uint32{}} : from.type();
  if (!(type == into.type)) {
    return base::ErrStatus(
        "row store: a column arrived holding one type after holding another");
  }
  return base::OkStatus();
}

void RowStore::AppendColumn(Column& into,
                            const ColumnView& from,
                            uint32_t count) {
  bool variant = from.kind() == ColumnView::Kind::kVariant;
  StorageType type = from.type();
  if (!variant && type.Is<Id>()) {
    type = StorageType{Uint32{}};
  }
  bool nullable = into.nullable || from.validity() != nullptr;
  if (nullable && !into.nullable) {
    // Rows appended while the column was non-nullable are all valid, but only
    // those: the unwritten tail of the last chunk has to stay clear because
    // the validity copy can only ever set bits, never clear one set here.
    for (uint32_t i = 0; i < into.chunks.size(); ++i) {
      if (!into.chunks[i]) {
        continue;
      }
      uint64_t filled =
          std::min<uint64_t>(kChunkRows, size_ - uint64_t{i} * kChunkRows);
      BitVector validity = BitVector::CreateWithSize(filled, true);
      validity.resize(kChunkRows);
      into.chunks[i]->validity = std::move(validity);
    }
  }

  for (uint32_t done = 0; done < count;) {
    uint32_t row = size_ + done;
    uint32_t index = row / kChunkRows;
    uint32_t at = row % kChunkRows;
    uint32_t run = std::min(count - done, kChunkRows - at);
    ColumnChunk& chunk = ChunkAt(into, index);
    if (variant) {
      Copy<Variant>(from, done, at, run, chunk);
    } else if (from.type().Is<Id>()) {
      CopyIds(from, done, at, run, chunk);
    } else if (type.Is<Uint32>()) {
      Copy<uint32_t>(from, done, at, run, chunk);
    } else if (type.Is<Int32>()) {
      Copy<int32_t>(from, done, at, run, chunk);
    } else if (type.Is<Int64>()) {
      Copy<int64_t>(from, done, at, run, chunk);
    } else if (type.Is<Double>()) {
      Copy<double>(from, done, at, run, chunk);
    } else {
      Copy<StringPool::Id>(from, done, at, run, chunk);
    }
    if (nullable) {
      if (chunk.validity.size() == 0) {
        chunk.validity = BitVector::CreateWithSize(kChunkRows);
      }
      CopyValidity(from, done, at, run, chunk.validity);
    }
    done += run;
  }
  into.variant = variant;
  into.type = type;
  into.nullable = nullable;
}

base::Status RowStore::Append(const RowBatch& batch) {
  if (!initialized_) {
    columns_.resize(batch.column_count());
  } else if (columns_.size() != batch.column_count()) {
    return base::ErrStatus(
        "row store: a batch of %u columns arrived after one of %u",
        batch.column_count(), static_cast<uint32_t>(columns_.size()));
  }
  for (uint32_t col = 0; col < columns_.size(); ++col) {
    RETURN_IF_ERROR(ValidateColumn(columns_[col], batch.column(col)));
  }
  for (uint32_t col = 0; col < columns_.size(); ++col) {
    AppendColumn(columns_[col], batch.column(col), batch.size());
  }
  size_ += batch.size();
  initialized_ = true;
  return base::OkStatus();
}

ColumnView RowStore::ViewOf(const Column& column,
                            const ColumnChunk& chunk) const {
  if (column.variant) {
    return ColumnView::Variants(chunk.Values<Variant>().data());
  }
  const void* data = nullptr;
  if (column.type.Is<Uint32>()) {
    data = chunk.Values<uint32_t>().data();
  } else if (column.type.Is<Int32>()) {
    data = chunk.Values<int32_t>().data();
  } else if (column.type.Is<Int64>()) {
    data = chunk.Values<int64_t>().data();
  } else if (column.type.Is<Double>()) {
    data = chunk.Values<double>().data();
  } else {
    data = chunk.Values<StringPool::Id>().data();
  }
  return ColumnView::Reference(column.type, data,
                               column.nullable ? &chunk.validity : nullptr);
}

uint32_t RowStore::View(RowBatch* batch,
                        uint32_t offset,
                        uint32_t count) const {
  PERFETTO_DCHECK(offset + count <= size_);
  batch->Reset();
  if (count == 0) {
    return 0;
  }
  uint32_t at = offset % kChunkRows;
  uint32_t served = std::min(count, kChunkRows - at);
  for (const Column& column : columns_) {
    ColumnView view = ViewOf(column, *column.chunks[offset / kChunkRows]);
    view.SetRange(at);
    batch->AddColumn(view);
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
      column.gathered = std::make_unique<ColumnChunk>();
    }
    ColumnChunk& into = *column.gathered;
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
    batch->AddColumn(view);
  }
  batch->SetCardinality(count);
}

void RowStore::Clear() {
  size_ = 0;
  for (Column& column : columns_) {
    column.nullable = false;
    for (const std::unique_ptr<ColumnChunk>& chunk : column.chunks) {
      if (chunk) {
        chunk->validity.clear();
      }
    }
  }
}

}  // namespace perfetto::trace_processor::core::exec
