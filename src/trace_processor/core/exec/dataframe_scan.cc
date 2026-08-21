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
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// A column which did not store one value per row, laid back out so that it
// does.
template <typename T>
struct Expanded {
  FlexVector<T> values;
  BitVector validity;
};

// Puts each value back at the row it belongs to. Rows holding nothing get the
// type's zero, so the storage is readable everywhere.
template <typename T>
std::shared_ptr<Expanded<T>> Expand(const T* packed,
                                    const BitVector& bits,
                                    uint32_t rows) {
  auto out = std::make_shared<Expanded<T>>();
  out->values = FlexVector<T>::CreateWithSize(rows);
  out->validity = BitVector::CreateWithSize(rows);
  uint32_t at = 0;
  for (uint32_t row = 0; row < rows; ++row) {
    if (bits.is_set(row)) {
      out->values[row] = packed[at++];
      out->validity.set(row);
    } else {
      out->values[row] = T{};
    }
  }
  return out;
}

template <typename T>
void Build(const dataframe::Column& column,
           StorageType type,
           uint32_t rows,
           ColumnView* view,
           std::shared_ptr<const void>* owner) {
  const T* data =
      column.storage
          .template unchecked_data<typename core::TypeTagFor<T>::type>();
  if (column.null_storage.nullability().template Is<core::NonNull>()) {
    *view = ColumnView::Reference(type, data, nullptr);
    return;
  }
  const BitVector& bits = column.null_storage.GetNullBitVector();
  if (column.null_storage.nullability().template Is<core::DenseNull>()) {
    // A slot per row already, so the values can be read where they lie.
    *view = ColumnView::Reference(type, data, &bits);
    return;
  }
  std::shared_ptr<Expanded<T>> expanded = Expand<T>(data, bits, rows);
  *view =
      ColumnView::Reference(type, expanded->values.data(), &expanded->validity);
  *owner = expanded;
}

}  // namespace

DataframeScan::DataframeScan(const dataframe::Dataframe* dataframe,
                             std::vector<uint32_t> columns)
    : dataframe_(dataframe), columns_(std::move(columns)) {}

DataframeScan::~DataframeScan() = default;
DataframeScan::State::~State() = default;

std::unique_ptr<OperatorState> DataframeScan::MakeState() const {
  auto state = std::make_unique<State>();
  uint32_t rows = dataframe_->row_count();
  state->columns.resize(columns_.size());
  state->owners.resize(columns_.size());
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    uint32_t index = columns_[i];
    StorageType type = dataframe_->column_type(index);
    if (type.Is<Id>()) {
      // No storage at all: the value is the row it sits at.
      state->columns[i] = ColumnView::Reference(type, nullptr, nullptr);
      continue;
    }
    const dataframe::Column& column = dataframe_->column(index);
    if (type.Is<Uint32>()) {
      Build<uint32_t>(column, type, rows, &state->columns[i],
                      &state->owners[i]);
    } else if (type.Is<Int32>()) {
      Build<int32_t>(column, type, rows, &state->columns[i], &state->owners[i]);
    } else if (type.Is<Int64>()) {
      Build<int64_t>(column, type, rows, &state->columns[i], &state->owners[i]);
    } else if (type.Is<Double>()) {
      Build<double>(column, type, rows, &state->columns[i], &state->owners[i]);
    } else {
      Build<StringPool::Id>(column, type, rows, &state->columns[i],
                            &state->owners[i]);
    }
  }
  return state;
}

void DataframeScan::Rewind(OperatorState& state) const {
  state.Cast<State>().emitted = 0;
}

bool DataframeScan::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t rows = dataframe_->row_count();
  if (s.emitted == rows) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, rows - s.emitted);
  out.Reset();
  for (uint32_t i = 0; i < s.columns.size(); ++i) {
    out.AddColumn(s.columns[i], s.owners[i]);
  }
  out.Compose(RowSelection::Range(s.emitted), count);
  out.SetCardinality(count);
  s.emitted += count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
