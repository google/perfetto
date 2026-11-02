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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_LIMIT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_LIMIT_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// Drops the first `offset` rows, then keeps up to `limit` rows, in order: SQL's
// LIMIT and OFFSET. Rows are dropped by narrowing the selection, so nothing is
// copied.
//
// Once it has kept `limit` rows it takes no more input, so nothing before it
// reads further than the rows it needs.
class Limit final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  // No `limit` keeps every row after the offset.
  Limit(std::optional<uint32_t> limit, uint32_t offset);
  ~Limit() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    State() : OperatorState(ResetEachRun{}) {}
    ~State() override;
    void Reset() override;

    // How many rows of the offset are dropped, and how many are kept.
    uint32_t skipped = 0;
    uint32_t kept = 0;
  };

  std::optional<uint32_t> limit_;
  uint32_t offset_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_LIMIT_H_
