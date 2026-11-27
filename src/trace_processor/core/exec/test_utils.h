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
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec::test {

// Adds to `out` a copy of `values`, present where `validity` says if not
// null, in a buffer from `context`: how a source refilling its storage each
// call publishes it.
template <typename T>
void AddCopy(Context& context,
             const std::vector<T>& values,
             const BitVector* validity,
             RowBatch* out) {
  ColumnBuffer buffer = context.TakeBuffer(sizeof(T));
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
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

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
    out.AddBorrowedColumn(ColumnView::Sequence(nullptr, s.emitted));
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
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

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
    out.AddBorrowedColumn(ColumnView::Sequence(nullptr, 0));
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

// Adds a pipeline streaming `input`, which must outlive `pipelines`, as a
// plan adds a side pipeline, and returns its id.
inline uint32_t AddStream(Pipelines& pipelines, const Source& input) {
  return pipelines.Add(
      nullptr, std::make_unique<Pipeline>(input, std::vector<Pipeline::Step>()),
      nullptr);
}

// Serves copies of `batches`, which must outlive it, in order.
class BatchesSource final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit BatchesSource(std::vector<const RowBatch*> batches)
      : batches_(std::move(batches)) {}

  std::unique_ptr<OperatorState> MakeState(Context&) const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().next = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.next == batches_.size()) {
      return false;
    }
    out.CopyFrom(*batches_[s.next++]);
    return true;
  }

 private:
  struct State : OperatorState {
    size_t next = 0;
  };
  std::vector<const RowBatch*> batches_;
};

// `breaker` run after `before` over `input`, and before `after`, as a plan
// runs it: the pipeline of `before` ends in it, and the pipeline of `after`
// starts with the source serving what it built. As pipitdb's after_breaker.
class AfterBreaker {
 public:
  AfterBreaker(const Source& input,
               std::vector<Pipeline::Step> before,
               std::unique_ptr<Breaker> breaker,
               std::vector<Pipeline::Step> after = {}) {
    const Breaker& served = *breaker;
    uint32_t id = pipelines_.Add(
        nullptr, std::make_unique<Pipeline>(input, std::move(before)),
        std::move(breaker));
    served_ = std::make_unique<Served>(id, served);
    pipeline_ = std::make_unique<Pipeline>(*served_, std::move(after));
    context_.SetPipelines(&pipelines_);
  }

  const Pipeline& pipeline() const { return *pipeline_; }
  const Pipelines& pipelines() const { return pipelines_; }
  Context& context() { return context_; }

  // Every batch it outputs, read by `read`, and how the run ended.
  template <typename Read>
  base::Status Drain(Read read) {
    std::unique_ptr<OperatorState> state = pipeline_->MakeState(context_);
    RowBatch batch;
    while (pipeline_->GetData(batch, *state)) {
      read(batch);
    }
    return pipeline_->status(*state);
  }

 private:
  Pipelines pipelines_;
  std::unique_ptr<Served> served_;
  std::unique_ptr<Pipeline> pipeline_;
  Context context_;
};

// Runs `breaker` over copies of `batches`, which must outlive it, each time
// Run() is called, as a plan runs it. For benchmarks.
class BreakerRun {
 public:
  BreakerRun(const std::vector<RowBatch>& batches,
             std::unique_ptr<Breaker> breaker)
      : source_(Pointers(batches)),
        run_(source_, {}, std::move(breaker)),
        state_(run_.pipeline().MakeState(run_.context())) {}

  // How many rows it output.
  uint64_t Run() {
    const Pipeline& pipeline = run_.pipeline();
    Restart(pipeline, *state_);
    uint64_t rows = 0;
    while (pipeline.GetData(out_, *state_)) {
      rows += out_.size();
    }
    return rows;
  }

 private:
  static std::vector<const RowBatch*> Pointers(
      const std::vector<RowBatch>& batches) {
    std::vector<const RowBatch*> out;
    for (const RowBatch& batch : batches) {
      out.push_back(&batch);
    }
    return out;
  }

  BatchesSource source_;
  AfterBreaker run_;
  std::unique_ptr<OperatorState> state_;
  RowBatch out_;
};

}  // namespace perfetto::trace_processor::core::exec::test

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_
