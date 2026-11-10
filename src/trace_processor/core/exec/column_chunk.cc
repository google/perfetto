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

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

static_assert(std::is_trivially_copyable_v<Variant>);
static_assert(sizeof(StringPool::Id) == sizeof(uint32_t));

// Calls `copy` with the width of `view`'s values, as a constant. Values are
// copied as bytes, so types of the same width share one copy loop.
template <typename Copy>
void DispatchOnWidth(const ColumnView& view, Copy copy) {
  switch (ValueWidth(view)) {
    case sizeof(uint32_t):
      copy(std::integral_constant<uint32_t, sizeof(uint32_t)>{});
      break;
    case sizeof(int64_t):
      copy(std::integral_constant<uint32_t, sizeof(int64_t)>{});
      break;
    default:
      copy(std::integral_constant<uint32_t, sizeof(Variant)>{});
      break;
  }
}

// Copies the values of `view` stored at `index(i)`, for each i below `count`,
// into `chunk` from `offset`. Each value is `kWidth` bytes.
template <uint32_t kWidth, typename Index>
void CopyIndexed(const ColumnView& view,
                 Index index,
                 uint32_t count,
                 uint32_t offset,
                 ColumnChunk& chunk) {
  uint8_t* dest = chunk.Values<uint8_t>() + uint64_t{offset} * kWidth;
  if constexpr (kWidth == sizeof(uint32_t)) {
    // A sequence column has no storage: each value is where it is stored.
    // Only Id columns are sequences.
    if (view.kind() == ColumnView::Kind::kSequence) {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t value = index(i);
        memcpy(dest + i * kWidth, &value, kWidth);
      }
      return;
    }
  }
  const auto* data = static_cast<const uint8_t*>(view.data());
  for (uint32_t i = 0; i < count; ++i) {
    memcpy(dest + i * kWidth, data + uint64_t{index(i)} * kWidth, kWidth);
  }
}

// Writes whether each of `count` rows holds a value into `validity` from
// `offset`, a word of bits at a time: row i's is `valid(i)`.
template <typename Valid>
void WriteValidity(Valid valid,
                   uint32_t count,
                   uint32_t offset,
                   BitVector& validity) {
  validity.FillBits(offset, count, false);
  for (uint32_t begin = 0; begin < count; begin += 64) {
    uint32_t n = std::min(64u, count - begin);
    uint64_t word = 0;
    for (uint32_t i = 0; i < n; ++i) {
      word |= uint64_t{valid(begin + i)} << i;
    }
    validity.OrBits(offset + begin, word, n);
  }
}

}  // namespace

void ColumnChunk::CopyFrom(const RowBatch& batch,
                           uint32_t column,
                           uint32_t offset) {
  const ColumnView& view = batch.column(column);
  PERFETTO_DCHECK(uint64_t{ValueWidth(view)} * kMaxBatchRows <= storage.size());
  const Selection& selection = batch.selection();
  uint32_t start = view.start();
  uint32_t count = batch.size();
  SizeValidity();
  const BitVector* source = view.validity();
  if (!selection.prefix()) {
    auto index = [&](uint32_t i) { return start + selection[i]; };
    DispatchOnWidth(view, [&](auto width) {
      CopyIndexed<decltype(width)::value>(view, index, count, offset, *this);
    });
    if (source) {
      WriteValidity([&](uint32_t i) { return source->is_set(index(i)); }, count,
                    offset, validity);
    } else {
      validity.FillBits(offset, count, true);
    }
    return;
  }
  // The rows kept are the window's first: one run of values.
  if (view.kind() == ColumnView::Kind::kSequence) {
    uint32_t* dest = Values<uint32_t>() + offset;
    for (uint32_t i = 0; i < count; ++i) {
      dest[i] = start + i;
    }
  } else {
    uint32_t width = ValueWidth(view);
    memcpy(Values<uint8_t>() + uint64_t{offset} * width,
           static_cast<const uint8_t*>(view.data()) + uint64_t{start} * width,
           uint64_t{count} * width);
  }
  if (source) {
    validity.FillBits(offset, count, false);
    validity.OrBitsFrom(offset, *source, start, count);
  } else {
    validity.FillBits(offset, count, true);
  }
}

void ColumnChunk::Gather(const ColumnView& like,
                         const GatherSource* sources,
                         const uint32_t* batches,
                         const uint32_t* rows,
                         uint32_t count,
                         bool nullable) {
  PERFETTO_DCHECK(uint64_t{ValueWidth(like)} * kMaxBatchRows <= storage.size());
  DispatchOnWidth(like, [&](auto width) {
    constexpr uint32_t kWidth = decltype(width)::value;
    uint8_t* dest = Values<uint8_t>();
    for (uint32_t i = 0; i < count; ++i) {
      const GatherSource& source = sources[batches[i]];
      uint32_t index = source.start + rows[i];
      if constexpr (kWidth == sizeof(uint32_t)) {
        // A sequence has no storage: its value is where it would be stored.
        // Only Id columns are sequences.
        if (PERFETTO_UNLIKELY(!source.data)) {
          memcpy(dest + i * kWidth, &index, kWidth);
          continue;
        }
      }
      memcpy(
          dest + i * kWidth,
          static_cast<const uint8_t*>(source.data) + uint64_t{index} * kWidth,
          kWidth);
    }
  });

  SizeValidity();
  if (!nullable) {
    validity.FillBits(0, count, true);
    return;
  }
  WriteValidity(
      [&](uint32_t i) {
        const GatherSource& source = sources[batches[i]];
        return !source.validity ||
               source.validity->is_set(source.start + rows[i]);
      },
      count, 0, validity);
}

void ColumnChunk::GrowValidity() {
  validity.resize(kMaxBatchRows);
}

ColumnView ColumnChunk::View(const ColumnView& source, bool nullable) const {
  if (source.kind() == ColumnView::Kind::kVariant) {
    return ColumnView::Variants(Values<Variant>());
  }
  auto type = source.type().Is<Id>() ? StorageType{Uint32{}} : source.type();
  return ColumnView::Reference(type, storage.data(),
                               nullable ? &validity : nullptr);
}
}  // namespace perfetto::trace_processor::core::exec
