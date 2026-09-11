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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec::test {

template <typename T>
std::vector<T> ReadColumn(const RowBatch& batch, uint32_t column) {
  const ColumnView& view = batch.column(column);
  std::vector<T> values;
  values.reserve(batch.size());
  for (uint32_t row = 0; row < batch.size(); ++row) {
    values.push_back(view.Value<T>(row));
  }
  return values;
}

template <typename T>
std::vector<std::optional<T>> ReadNullableColumn(const RowBatch& batch,
                                                 uint32_t column) {
  const ColumnView& view = batch.column(column);
  const BitVector* validity = view.validity();
  std::vector<std::optional<T>> values;
  values.reserve(batch.size());
  for (uint32_t row = 0; row < batch.size(); ++row) {
    uint32_t index = view.selection().GetIndex(row);
    if (validity && !validity->is_set(index)) {
      values.emplace_back(std::nullopt);
    } else {
      values.emplace_back(view.Value<T>(row));
    }
  }
  return values;
}

// A source shaped like a dataframe scan: an id column so every batch carries
// its row indices, plus the values themselves.
class ArraySource final : public Source {
 public:
  explicit ArraySource(std::vector<int64_t> values)
      : values_(std::move(values)) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }

  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(values_.size());
    if (s.emitted == rows) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, rows - s.emitted);
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values_.data()));
    out.Compose(RowSelection::Range(s.emitted), count);
    out.SetCardinality(count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };

  std::vector<int64_t> values_;
};

inline std::vector<int64_t> Sequence(uint32_t count) {
  std::vector<int64_t> values(count);
  for (uint32_t i = 0; i < count; ++i) {
    values[i] = i;
  }
  return values;
}

// Emits one batch, then fails.
class FailingSource final : public Source {
 public:
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = false;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.emitted) {
      return false;
    }
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values_));
    out.SetCardinality(2);
    s.emitted = true;
    return true;
  }
  base::Status status(const OperatorState& state) const override {
    return state.Cast<const State>().emitted ? base::ErrStatus("input broke")
                                             : base::OkStatus();
  }

 private:
  struct State : OperatorState {
    bool emitted = false;
  };
  int64_t values_[2] = {1, 2};
};

}  // namespace perfetto::trace_processor::core::exec::test

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_
