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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_

#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// An operator which buffers input before producing output. Pipeline drives it
// exactly like any other operator: Execute() consumes batches, then Finish()
// prepares the buffered data once and serves it over successive calls.
class Breaker : public Operator {
 public:
  struct State : OperatorState {
    ~State() override;

    base::Status status = base::OkStatus();
    bool filled = false;
  };

  ~Breaker() override;

  std::unique_ptr<OperatorState> MakeState() const final;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const final;
  OpResult Finish(RowBatch& out, OperatorState& state) const final;
  void Rewind(OperatorState& state) const final;
  base::Status status(const OperatorState& state) const final;

 protected:
  Breaker() = default;

  // Creates the state for the buffered data.
  virtual std::unique_ptr<State> CreateState() const = 0;
  // The sink half. Each batch of the input goes into Consume(), then Finalize()
  // runs once. Both return false on failure, having set the status.
  virtual bool Consume(const RowBatch& in, State& state) const = 0;
  virtual bool Finalize(State& state) const = 0;
  // The source half: fills `out` from what was consumed, or returns false
  // when nothing is left or on failure.
  virtual bool Serve(RowBatch& out, State& state) const = 0;
  // Drops everything consumed so the input can be read again.
  virtual void Reset(State& state) const = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
