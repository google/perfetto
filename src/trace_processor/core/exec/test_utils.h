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
#include "perfetto/ext/base/no_destructor.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec::test {

// A Context for tests which make states themselves, rather than through a
// RowCursor. Tests run on one thread, so they can share it.
inline Context& TestContext() {
  static base::NoDestructor<Context> context;
  return context.ref();
}

// Adds to `out` a copy of `values`, present where `validity` says if not
// null, in a buffer from `context`: how a source refilling its storage each
// call publishes it.
template <typename T>
void AddCopy(Context& context,
             const std::vector<T>& values,
             const BitVector* validity,
             RowBatch* out) {
  ColumnBuffer buffer = context.TakeBuffer();
  ColumnView view = buffer.chunk().Fill(values.data(), validity,
                                        static_cast<uint32_t>(values.size()));
  out->AddColumn(view, std::move(buffer));
}

// Moves every column's window `first` rows on, and gives the batch `count`
// rows, all kept.
inline void Window(RowBatch* batch, uint32_t first, uint32_t count) {
  for (uint32_t c = 0; c < batch->column_count(); ++c) {
    ColumnView column = batch->column(c);
    column.set_start(column.start() + first);
    batch->SetColumn(c, column, batch->buffer(c));
  }
  batch->SetRowCount(count);
}

template <typename T>
std::vector<T> ReadColumn(const RowBatch& batch, uint32_t column) {
  std::vector<T> values;
  values.reserve(batch.size());
  for (uint32_t row = 0; row < batch.size(); ++row) {
    values.push_back(batch.Value<T>(column, row));
  }
  return values;
}

template <typename T>
std::vector<std::optional<T>> ReadNullableColumn(const RowBatch& batch,
                                                 uint32_t column) {
  std::vector<std::optional<T>> values;
  values.reserve(batch.size());
  for (uint32_t row = 0; row < batch.size(); ++row) {
    if (!batch.IsValid(column, row)) {
      values.emplace_back(std::nullopt);
    } else {
      values.emplace_back(batch.Value<T>(column, row));
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

  std::unique_ptr<OperatorState> MakeState(Context&) const override {
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
    out.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr, s.emitted));
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, values_.data(), nullptr, s.emitted));
    out.SetRowCount(count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };

  std::vector<int64_t> values_;
};

// Runs `transform` on a copy of `in` made in `out`, as a pipeline hands each
// batch over afresh; false if it failed.
inline bool ProcessCopy(const Transform& transform,
                        const RowBatch& in,
                        RowBatch& out,
                        OperatorState& state) {
  out.CopyFrom(in);
  return transform.Process(out, state);
}

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
  std::unique_ptr<OperatorState> MakeState(Context&) const override {
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
    out.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
    out.AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}}, values_));
    out.SetRowCount(2);
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
