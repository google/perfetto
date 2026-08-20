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
#include "src/trace_processor/core/exec/sink.h"

namespace perfetto::trace_processor::core::exec {

// A breaker's execution. Everything it accumulates lives here.
class BreakerState : public OperatorState {
 public:
  ~BreakerState() override;

  // A breaker drains its input, so it is the thing that runs it.
  std::unique_ptr<OperatorState> input;
  RowBatch input_batch;
  bool filled = false;
  base::Status status = base::OkStatus();
};

// A step which cannot answer until it has seen everything: a sink while it
// fills and a source once it is full.
//
// Where the breakers are is where a pipeline stops being a pipeline. The
// memory a query needs at once and the latency before its first row are
// decided by them and by nothing else.
class Breaker : public Source, public Sink {
 public:
  ~Breaker() override;

  std::unique_ptr<OperatorState> MakeState() const final;
  void Rewind(OperatorState&) const final;
  bool GetData(RowBatch& out, OperatorState&) const final;
  base::Status status(const OperatorState&) const final;

 protected:
  explicit Breaker(const Source& input) : input_(input) {}

  // Must derive from BreakerState; the input's state is filled in after.
  virtual std::unique_ptr<BreakerState> MakeBreakerState() const = 0;

  // The next chunk of the answer, or false once there are none.
  virtual bool Emit(RowBatch& out, BreakerState&) const = 0;

  // Drops whatever was built.
  virtual void Rewind(BreakerState&) const = 0;

 private:
  base::Status Fill(BreakerState&) const;

  const Source& input_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
