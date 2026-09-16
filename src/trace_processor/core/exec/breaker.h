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

// A sink for one pipeline and the source of the next.
//
// A plan is push all the way up: a Pipeline pushes each batch of its source
// through its operators. Only at the top is it pulled from. Some steps cannot
// be pushed through, because nothing can come out until everything has gone
// in; sorting is the classic case. A breaker is that step: the pipeline below
// pushes every batch into Consume(), Finish() runs once after the last, and
// only then does the pipeline above start pulling batches out.
//
// What drives the push is the first pull: GetData() drains the input before
// serving anything, so a breaker sits in a plan wherever a Source does.
class Breaker : public Source {
 public:
  struct State : OperatorState {
    ~State() override;

    std::unique_ptr<OperatorState> input;
    // The batch of the input being consumed.
    RowBatch batch;
    base::Status status = base::OkStatus();
    bool filled = false;
  };

  ~Breaker() override;

  // The sink half. Each batch of the input goes into Consume(), then Finish()
  // runs once. Both return false on failure, having set the status.
  virtual bool Consume(const RowBatch& in, State& state) const = 0;
  virtual bool Finish(State& state) const = 0;

  std::unique_ptr<OperatorState> MakeState() const final;
  bool GetData(RowBatch& out, OperatorState& state) const final;
  void Rewind(OperatorState& state) const final;
  base::Status status(const OperatorState& state) const final;

 protected:
  explicit Breaker(const Source& input);
  Breaker(Source&&) = delete;

  // Creates the state; the base fills in what it owns.
  virtual std::unique_ptr<State> CreateState() const = 0;
  // The source half: fills `out` from what was consumed, or returns false
  // when nothing is left or on failure.
  virtual bool Serve(RowBatch& out, State& state) const = 0;
  // Drops everything consumed so the input can be read again.
  virtual void Reset(State& state) const = 0;

 private:
  const Source& input_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
