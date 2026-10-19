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

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {

// Lays a window of a column which does not store one value per row back out
// so that it does. The buffer is a batch wide and reused, so a column costs
// one window of work at a time rather than the whole column up front.
class DataframeWindows::Expander {
 public:
  virtual ~Expander();

  // Sets `view` to rows [from, from + count), laid out densely from zero, in
  // a buffer taken from `context` into `buffer`.
  virtual void Expand(uint32_t from,
                      uint32_t count,
                      Context& context,
                      ColumnView* view,
                      ColumnBuffer* buffer) = 0;

  // As Expand, but writes only the rows `offsets` from `from`, the first of
  // which is 0.
  virtual void ExpandKept(uint32_t from,
                          Span<const uint32_t> offsets,
                          Context& context,
                          ColumnView* view,
                          ColumnBuffer* buffer) = 0;
};

DataframeWindows::Expander::~Expander() = default;

namespace {

// Expands a column whose values are kWidth bytes. Values are copied as bytes,
// so types of the same width share one expander.
template <uint32_t kWidth>
class ExpanderImpl final : public DataframeWindows::Expander {
 public:
  // `packed` is null for an Id column, whose values are their positions.
  ExpanderImpl(StorageType type, const void* packed, const BitVector* bits)
      : type_(type), packed_(static_cast<const uint8_t*>(packed)), bits_(bits) {
    PERFETTO_DCHECK(packed_ || kWidth == sizeof(uint32_t));
  }

  void Expand(uint32_t from,
              uint32_t count,
              Context& context,
              ColumnView* view,
              ColumnBuffer* buffer) override {
    if (count == 1 && packed_) {
      LendOne(from, view, buffer);
      return;
    }
    Skip(from);
    *buffer = context.TakeBuffer();
    ColumnChunk& chunk = buffer->chunk();
    uint8_t* values = chunk.Values<uint8_t>();
    chunk.SizeValidity();
    chunk.validity.ClearAllBits();
    for (uint32_t row = 0; row < count; ++row) {
      uint8_t* value = values + uint64_t{row} * kWidth;
      if (bits_->is_set(from + row)) {
        if (packed_) {
          memcpy(value, packed_ + uint64_t{consumed_} * kWidth, kWidth);
        } else {
          memcpy(value, &consumed_, sizeof(consumed_));
        }
        ++consumed_;
        chunk.validity.set(row);
      } else {
        // Written even for a null row, so the storage is readable everywhere:
        // zero bytes are zero, 0.0 and the null string.
        memset(value, 0, kWidth);
      }
    }
    next_ = from + count;
    *view = ColumnView::Reference(type_, values, &chunk.validity);
  }
  void ExpandKept(uint32_t from,
                  Span<const uint32_t> offsets,
                  Context& context,
                  ColumnView* view,
                  ColumnBuffer* buffer) override {
    if (offsets.size() == 1 && packed_) {
      LendOne(from, view, buffer);
      return;
    }
    *buffer = context.TakeBuffer();
    ColumnChunk& chunk = buffer->chunk();
    uint8_t* values = chunk.Values<uint8_t>();
    chunk.SizeValidity();
    chunk.validity.ClearAllBits();
    for (const uint32_t* it = offsets.b; it != offsets.e; ++it) {
      uint32_t row = from + *it;
      uint8_t* value = values + uint64_t{*it} * kWidth;
      if (!bits_->is_set(row)) {
        memset(value, 0, kWidth);
        continue;
      }
      uint32_t slot = SlotOf(row);
      if (packed_) {
        memcpy(value, packed_ + uint64_t{slot} * kWidth, kWidth);
      } else {
        memcpy(value, &slot, sizeof(slot));
      }
      chunk.validity.set(*it);
    }
    *view = ColumnView::Reference(type_, values, &chunk.validity);
  }

 private:
  // Lends `row`'s value where it lies, or a null: one row needs no buffer.
  void LendOne(uint32_t row, ColumnView* view, ColumnBuffer* buffer) {
    *buffer = ColumnBuffer();
    if (!bits_->is_set(row)) {
      *view = ColumnView::Reference(type_, packed_, &one_null_);
      return;
    }
    Skip(row);
    *view = ColumnView::Reference(type_, packed_ + uint64_t{consumed_} * kWidth,
                                  nullptr);
    ++consumed_;
    ++next_;
  }

  void Skip(uint32_t row) {
    if (row == next_) {
      return;
    }
    consumed_ = SlotOf(row);
    next_ = row;
  }

  // How many packed values are before `row`: O(1) once the prefix popcount
  // is built.
  uint32_t SlotOf(uint32_t row) {
    if (PERFETTO_UNLIKELY(!popcount_.size())) {
      popcount_ = bits_->PrefixPopcount();
    }
    return popcount_[row / 64] +
           static_cast<uint32_t>(bits_->count_set_bits_until_in_word(row));
  }

  StorageType type_;
  const uint8_t* packed_;
  const BitVector* bits_;
  // How many of the packed values are before `next_`.
  uint32_t consumed_ = 0;
  uint32_t next_ = 0;
  // Built on the first SlotOf().
  Slab<uint32_t> popcount_;
  BitVector one_null_ = BitVector::CreateWithSize(1);
};

// The expander for a column of `type` whose values are `width` bytes.
std::unique_ptr<DataframeWindows::Expander> MakeExpander(
    uint32_t width,
    StorageType type,
    const void* packed,
    const BitVector* bits) {
  if (width == sizeof(uint32_t)) {
    return std::make_unique<ExpanderImpl<sizeof(uint32_t)>>(type, packed, bits);
  }
  PERFETTO_DCHECK(width == sizeof(uint64_t));
  return std::make_unique<ExpanderImpl<sizeof(uint64_t)>>(type, packed, bits);
}

// Sets `view` straight onto the dataframe's storage or, for a column without
// a slot per row, returns the expander which fills one window of it.
template <typename T>
std::unique_ptr<DataframeWindows::Expander> BuildColumn(
    const dataframe::Column& column,
    StorageType type,
    ColumnView* view) {
  const T* data =
      column.storage
          .template unchecked_data<typename core::TypeTagFor<T>::type>();
  const auto& nulls = column.null_storage;
  if (nulls.nullability().template Is<core::NonNull>()) {
    *view = ColumnView::Reference(type, data, nullptr);
    return nullptr;
  }
  const BitVector& bits = nulls.GetNullBitVector();
  if (nulls.nullability().template Is<core::DenseNull>()) {
    // Already one slot per row, so the values can be read where they lie.
    *view = ColumnView::Reference(type, data, &bits);
    return nullptr;
  }
  return MakeExpander(sizeof(T), type, data, &bits);
}

}  // namespace

DataframeWindows::DataframeWindows() = default;
DataframeWindows::~DataframeWindows() = default;

void DataframeWindows::Init(
    const std::vector<std::shared_ptr<const dataframe::Column>>& columns,
    Context& context) {
  context_ = &context;
  PERFETTO_DCHECK(lent_.empty());
  lent_.resize(columns.size());
  buffers_.resize(columns.size());
  for (uint32_t i = 0; i < columns.size(); ++i) {
    const dataframe::Column& column = *columns[i];
    StorageType type = column.storage.type();
    std::unique_ptr<Expander> expander;
    if (type.Is<Id>()) {
      const auto& nulls = column.null_storage;
      if (nulls.nullability().Is<NonNull>()) {
        lent_[i] = ColumnView::Reference(type, nullptr, nullptr);
      } else if (nulls.nullability().Is<DenseNull>()) {
        lent_[i] =
            ColumnView::Reference(type, nullptr, &nulls.GetNullBitVector());
      } else {
        expander = MakeExpander(sizeof(uint32_t), StorageType{Uint32{}},
                                nullptr, &nulls.GetNullBitVector());
      }
    } else if (type.Is<Uint32>()) {
      expander = BuildColumn<uint32_t>(column, type, &lent_[i]);
    } else if (type.Is<Int32>()) {
      expander = BuildColumn<int32_t>(column, type, &lent_[i]);
    } else if (type.Is<Int64>()) {
      expander = BuildColumn<int64_t>(column, type, &lent_[i]);
    } else if (type.Is<Double>()) {
      expander = BuildColumn<double>(column, type, &lent_[i]);
    } else {
      expander = BuildColumn<StringPool::Id>(column, type, &lent_[i]);
    }
    if (expander) {
      expanded_.push_back({i, std::move(expander)});
    }
  }
}

void DataframeWindows::FillKept(uint32_t from,
                                Span<const uint32_t> offsets,
                                RowBatch& out) {
  for (ColumnView& view : lent_) {
    view.set_start(from);
  }
  for (Expanded& e : expanded_) {
    e.expander->ExpandKept(from, offsets, *context_, &lent_[e.column],
                           &buffers_[e.column]);
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
  for (Expanded& e : expanded_) {
    e.expander->Expand(from, count, *context_, &lent_[e.column],
                       &buffers_[e.column]);
  }
  out.SetColumns(lent_, buffers_);
  out.SetRowCount(count);
}

}  // namespace perfetto::trace_processor::core::exec
