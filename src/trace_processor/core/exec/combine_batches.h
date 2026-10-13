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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_COMBINE_BATCHES_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_COMBINE_BATCHES_H_

#include <cstdint>
#include <memory>

#include "src/trace_processor/core/exec/batch_buffer.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// Copies the rows of small batches into larger ones. Used before operators
// that work best on full batches. A batch with more than kSmallBatchRows rows
// passes through unchanged. Row order is kept.
class CombineBatches final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // Batches with at most this many rows are combined.
  static constexpr uint32_t kSmallBatchRows = 64;
  // A combined batch is output once it has this many rows.
  static constexpr uint32_t kTargetBatchRows = kMaxBatchRows;

  CombineBatches();
  ~CombineBatches() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;
  OpResult Finish(RowBatch& out, OperatorState& state) const override;

 private:
  struct State : OperatorState {
    explicit State(Context& c) : OperatorState(ResetEachRun{}), context(&c) {}
    ~State() override;
    void Reset() override;

    // The buffered rows.
    BatchBuffer buffered;

    // Whether the buffered rows were already output for the current input
    // batch.
    bool flushed = false;

    // Provides the buffers combined rows are copied into.
    Context* context;
  };
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COMBINE_BATCHES_H_
