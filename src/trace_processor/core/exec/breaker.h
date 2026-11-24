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
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/sink.h"

namespace perfetto::trace_processor::core::exec {

// Holds back every row of its input, then outputs rows made from them, such
// as a sort or an aggregation: the sink of the pipeline it consumes, and
// served by the source of the next (see Served).
class Breaker : public Sink {
 public:
  struct State : OperatorState {
    State() = default;
    ~State() override;

    // What the columns served are filled in.
    Context* context = nullptr;
    Held<base::Status> status{this};
  };

  ~Breaker() override;

  // As Source::InPlace(), for the batches Serve() outputs.
  virtual const RowsInPlace* InPlace(const OperatorState&) const {
    return nullptr;
  }

  // Sink.
  std::unique_ptr<OperatorState> MakeSinkState(Context&) const final;
  base::Status ConsumeBatch(const RowBatch& in,
                            OperatorState& state) const final;
  base::Status FinalizeSink(OperatorState& state) const final;

 protected:
  Breaker() = default;

 private:
  friend class Served;

  // Creates the state for the buffered data.
  virtual std::unique_ptr<State> CreateState() const = 0;

  // Each batch of the input goes into Consume(), then Finalize() runs once.
  // Both return false on failure, having set the status.
  virtual bool Consume(const RowBatch& in, State& state) const = 0;
  virtual bool Finalize(State& state) const = 0;

  // Fills `out` from what was consumed, or returns false when nothing is
  // left or on failure.
  virtual bool Serve(RowBatch& out, State& state) const = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
