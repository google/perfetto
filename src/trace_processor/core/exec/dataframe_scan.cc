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

#include "src/trace_processor/core/exec/dataframe_scan.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
// Lays a batch's worth of a column which does not store one value per row back
// out so that it does. The buffer is a batch wide and reused, so a scan of a
// sparse column costs one batch of work at a time rather than the whole column
// up front.
class DataframeScan::Expander {
 public:
  virtual ~Expander();

  // Sets `view` to rows [from, from + count), laid out densely from zero, in
  // a buffer taken from `context` into `buffer`. Called with successive
  // ranges starting at row zero.
  virtual void Expand(uint32_t from,
                      uint32_t count,
                      Context& context,
                      ColumnView* view,
                      ColumnBuffer* buffer) = 0;

  virtual void Rewind() = 0;
};

DataframeScan::Expander::~Expander() = default;

namespace {

template <typename T>
class ExpanderImpl final : public DataframeScan::Expander {
 public:
  ExpanderImpl(StorageType type, const T* packed, const BitVector* bits)
      : type_(type), packed_(packed), bits_(bits) {}

  void Expand(uint32_t from,
              uint32_t count,
              Context& context,
              ColumnView* view,
              ColumnBuffer* buffer) override {
    PERFETTO_DCHECK(from == next_);
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
  StorageType type_;
  const T* packed_;
  const BitVector* bits_;
  // How many of the packed values have been read, which is how many rows
  // before `next_` hold one.
  uint32_t consumed_ = 0;
  uint32_t next_ = 0;
};

// Builds either a view straight onto the dataframe's storage or, for a column
// without a slot per row, the expander which fills one batch of it.
template <typename T>
void BuildColumn(const dataframe::Column& column,
                 StorageType type,
                 ColumnView* view,
                 std::unique_ptr<DataframeScan::Expander>* expander) {
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

DataframeScan::DataframeScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    uint32_t row_count)
    : columns_(std::move(columns)), row_count_(row_count) {}

DataframeScan::~DataframeScan() = default;
DataframeScan::State::~State() = default;

std::unique_ptr<OperatorState> DataframeScan::MakeState(
    Context& context) const {
  auto state = std::make_unique<State>();
  state->context = &context;
  state->lent.resize(columns_.size());
  std::vector<std::unique_ptr<Expander>> expanders(columns_.size());
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    const dataframe::Column& column = *columns_[i];
    StorageType type = column.storage.type();
    if (type.Is<Id>()) {
      const auto& nulls = column.null_storage;
      if (nulls.nullability().Is<NonNull>()) {
        state->lent[i] = ColumnView::Reference(type, nullptr, nullptr);
      } else if (nulls.nullability().Is<DenseNull>()) {
        state->lent[i] =
            ColumnView::Reference(type, nullptr, &nulls.GetNullBitVector());
      } else {
        expanders[i] = std::make_unique<ExpanderImpl<uint32_t>>(
            StorageType{Uint32{}}, nullptr, &nulls.GetNullBitVector());
      }
      continue;
    }
    if (type.Is<Uint32>()) {
      BuildColumn<uint32_t>(column, type, &state->lent[i], &expanders[i]);
    } else if (type.Is<Int32>()) {
      BuildColumn<int32_t>(column, type, &state->lent[i], &expanders[i]);
    } else if (type.Is<Int64>()) {
      BuildColumn<int64_t>(column, type, &state->lent[i], &expanders[i]);
    } else if (type.Is<Double>()) {
      BuildColumn<double>(column, type, &state->lent[i], &expanders[i]);
    } else {
      BuildColumn<StringPool::Id>(column, type, &state->lent[i], &expanders[i]);
    }
  }
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    if (expanders[i]) {
      state->expanded.push_back({i, std::move(expanders[i])});
    }
  }
  if (!state->expanded.empty()) {
    state->buffers.resize(columns_.size());
  }
  return state;
}

void DataframeScan::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  s.emitted = 0;
  for (State::Expanded& expanded : s.expanded) {
    expanded.expander->Rewind();
  }
}

bool DataframeScan::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t rows = row_count_;
  if (s.emitted == rows) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, rows - s.emitted);
  // Only the windows move: the views stay as they were lent.
  for (ColumnView& view : s.lent) {
    view.set_start(s.emitted);
  }
  for (State::Expanded& expanded : s.expanded) {
    expanded.expander->Expand(s.emitted, count, *s.context,
                              &s.lent[expanded.column],
                              &s.buffers[expanded.column]);
  }
  out.SetColumns(s.lent, s.buffers);
  out.SetRowCount(count);
  s.emitted += count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
