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
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
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

  // Appends an owned column containing rows [from, from + count), laid out
  // densely from zero. Called with successive ranges starting at row zero.
  virtual void Expand(uint32_t from, uint32_t count, RowBatch& out) = 0;

  virtual void Rewind() = 0;
};

DataframeScan::Expander::~Expander() = default;

namespace {

template <typename T>
class ExpanderImpl final : public DataframeScan::Expander {
 public:
  ExpanderImpl(StorageType type, const T* packed, const BitVector* bits)
      : type_(type), packed_(packed), bits_(bits) {}

  void Expand(uint32_t from, uint32_t count, RowBatch& out) override {
    PERFETTO_DCHECK(from == next_);
    auto buffer = buffers_.Acquire();
    buffer->values.resize(kMaxBatchRows);
    buffer->validity.resize(kMaxBatchRows);
    buffer->validity.ClearAllBits();
    for (uint32_t row = 0; row < count; ++row) {
      if (bits_->is_set(from + row)) {
        if constexpr (std::is_same_v<T, uint32_t>) {
          buffer->values[row] =
              packed_ ? packed_[consumed_] : static_cast<uint32_t>(consumed_);
        } else {
          PERFETTO_DCHECK(packed_);
          buffer->values[row] = packed_[consumed_];
        }
        ++consumed_;
        buffer->validity.set(row);
      } else {
        // Written even for a null row, so the storage is readable everywhere.
        buffer->values[row] = T{};
      }
    }
    next_ = from + count;
    auto view =
        ColumnView::Reference(type_, buffer->values.data(), &buffer->validity);
    out.AddColumn(std::move(view), std::move(buffer));
  }

  void Rewind() override {
    consumed_ = 0;
    next_ = 0;
  }

 private:
  struct Buffer {
    FlexVector<T> values;
    BitVector validity;
  };

  StorageType type_;
  const T* packed_;
  const BitVector* bits_;
  BufferPool<Buffer> buffers_;
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

std::unique_ptr<OperatorState> DataframeScan::MakeState() const {
  auto state = std::make_unique<State>();
  state->columns.resize(columns_.size());
  state->expanders.resize(columns_.size());
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    const dataframe::Column& column = *columns_[i];
    StorageType type = column.storage.type();
    if (type.Is<Id>()) {
      const auto& nulls = column.null_storage;
      if (nulls.nullability().Is<NonNull>()) {
        state->columns[i] = ColumnView::Reference(type, nullptr, nullptr);
      } else if (nulls.nullability().Is<DenseNull>()) {
        state->columns[i] =
            ColumnView::Reference(type, nullptr, &nulls.GetNullBitVector());
      } else {
        state->expanders[i] = std::make_unique<ExpanderImpl<uint32_t>>(
            StorageType{Uint32{}}, nullptr, &nulls.GetNullBitVector());
      }
      continue;
    }
    if (type.Is<Uint32>()) {
      BuildColumn<uint32_t>(column, type, &state->columns[i],
                            &state->expanders[i]);
    } else if (type.Is<Int32>()) {
      BuildColumn<int32_t>(column, type, &state->columns[i],
                           &state->expanders[i]);
    } else if (type.Is<Int64>()) {
      BuildColumn<int64_t>(column, type, &state->columns[i],
                           &state->expanders[i]);
    } else if (type.Is<Double>()) {
      BuildColumn<double>(column, type, &state->columns[i],
                          &state->expanders[i]);
    } else {
      BuildColumn<StringPool::Id>(column, type, &state->columns[i],
                                  &state->expanders[i]);
    }
  }
  return state;
}

void DataframeScan::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  s.emitted = 0;
  for (const std::unique_ptr<Expander>& expander : s.expanders) {
    if (expander) {
      expander->Rewind();
    }
  }
}

bool DataframeScan::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t rows = row_count_;
  if (s.emitted == rows) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, rows - s.emitted);
  out.Reset();
  for (uint32_t i = 0; i < s.columns.size(); ++i) {
    ColumnView view = s.columns[i];
    if (s.expanders[i]) {
      // Expanded values are laid out from zero, so the column sits in its own
      // index space rather than the dataframe's.
      s.expanders[i]->Expand(s.emitted, count, out);
    } else {
      view.SetRange(s.emitted);
      out.AddColumn(std::move(view), columns_[i]);
    }
  }
  out.SetCardinality(count);
  s.emitted += count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
