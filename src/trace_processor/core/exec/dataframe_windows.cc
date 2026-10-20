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
#include <memory>
#include <type_traits>
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
  // a buffer taken from `context` into `buffer`. `from` is at or after where
  // the last call ended.
  virtual void Expand(uint32_t from,
                      uint32_t count,
                      Context& context,
                      ColumnView* view,
                      ColumnBuffer* buffer) = 0;

  virtual void Rewind() = 0;
};

DataframeWindows::Expander::~Expander() = default;

namespace {

template <typename T>
class ExpanderImpl final : public DataframeWindows::Expander {
 public:
  ExpanderImpl(StorageType type, const T* packed, const BitVector* bits)
      : type_(type), packed_(packed), bits_(bits) {}

  void Expand(uint32_t from,
              uint32_t count,
              Context& context,
              ColumnView* view,
              ColumnBuffer* buffer) override {
    PERFETTO_DCHECK(from >= next_);
    if (from != next_) {
      Skip(from);
    }
    *buffer = context.TakeBuffer();
    ColumnChunk& chunk = buffer->chunk();
    T* values = chunk.Values<T>();
    chunk.validity.resize(kMaxBatchRows);
    chunk.validity.ClearAllBits();
    for (uint32_t row = 0; row < count; ++row) {
      if (bits_->is_set(from + row)) {
        if constexpr (std::is_same_v<T, uint32_t>) {
          values[row] =
              packed_ ? packed_[consumed_] : static_cast<uint32_t>(consumed_);
        } else {
          PERFETTO_DCHECK(packed_);
          values[row] = packed_[consumed_];
        }
        ++consumed_;
        chunk.validity.set(row);
      } else {
        // Written even for a null row, so the storage is readable everywhere.
        values[row] = T{};
      }
    }
    next_ = from + count;
    *view = ColumnView::Reference(type_, values, &chunk.validity);
  }

  void Rewind() override {
    consumed_ = 0;
    next_ = 0;
  }

 private:
  // Moves to row `row`, past the values of the rows before it, in constant
  // time once the bits set before each word are known.
  void Skip(uint32_t row) {
    if (PERFETTO_UNLIKELY(!popcount_.size())) {
      popcount_ = bits_->PrefixPopcount();
    }
    consumed_ = popcount_[row / 64] +
                static_cast<uint32_t>(bits_->count_set_bits_until_in_word(row));
    next_ = row;
  }

  StorageType type_;
  const T* packed_;
  const BitVector* bits_;
  // How many of the packed values are before `next_`.
  uint32_t consumed_ = 0;
  uint32_t next_ = 0;
  // By word, the bits set before it, once a window has started further on.
  Slab<uint32_t> popcount_;
};

// Builds either a view straight onto the dataframe's storage or, for a column
// without a slot per row, the expander which fills one window of it.
template <typename T>
void BuildColumn(const dataframe::Column& column,
                 StorageType type,
                 ColumnView* view,
                 std::unique_ptr<DataframeWindows::Expander>* expander) {
  const T* data =
      column.storage
          .template unchecked_data<typename core::TypeTagFor<T>::type>();
  const auto& nulls = column.null_storage;
  if (nulls.nullability().template Is<core::NonNull>()) {
    *view = ColumnView::Reference(type, data, nullptr);
    return;
  }
  const BitVector& bits = nulls.GetNullBitVector();
  if (nulls.nullability().template Is<core::DenseNull>()) {
    // Already one slot per row, so the values can be read where they lie.
    *view = ColumnView::Reference(type, data, &bits);
    return;
  }
  *expander = std::make_unique<ExpanderImpl<T>>(type, data, &bits);
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
        expander = std::make_unique<ExpanderImpl<uint32_t>>(
            type, nullptr, &nulls.GetNullBitVector());
      }
    } else if (type.Is<Uint32>()) {
      BuildColumn<uint32_t>(column, type, &lent_[i], &expander);
    } else if (type.Is<Int32>()) {
      BuildColumn<int32_t>(column, type, &lent_[i], &expander);
    } else if (type.Is<Int64>()) {
      BuildColumn<int64_t>(column, type, &lent_[i], &expander);
    } else if (type.Is<Double>()) {
      BuildColumn<double>(column, type, &lent_[i], &expander);
    } else {
      BuildColumn<StringPool::Id>(column, type, &lent_[i], &expander);
    }
    if (expander) {
      expanded_.push_back({i, std::move(expander)});
    }
  }
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

void DataframeWindows::Rewind() {
  for (Expanded& e : expanded_) {
    e.expander->Rewind();
  }
}

}  // namespace perfetto::trace_processor::core::exec
