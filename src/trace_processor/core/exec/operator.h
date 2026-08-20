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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_

#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Everything one execution of a plan is up to.
//
// A plan says what to do and never changes while it is being done; a state is
// where a particular doing of it has got to. Splitting them is what makes the
// ownership question have one answer: a plan holds no values, every value a
// query holds is in some state, and the states belong to whoever is running
// the plan rather than to the plan.
//
// It is also what makes a plan reusable. Running the same plan twice, or
// twice at once, is two states and one plan, so nothing has to be rebuilt and
// nothing can be left over from last time.
class OperatorState {
 public:
  OperatorState() = default;
  virtual ~OperatorState();

  OperatorState(const OperatorState&) = delete;
  OperatorState& operator=(const OperatorState&) = delete;

  // An operator only ever sees a state it made itself, so it may say which
  // one that was.
  template <typename T>
  T& Cast() {
    return static_cast<T&>(*this);
  }
  template <typename T>
  const T& Cast() const {
    return static_cast<const T&>(*this);
  }
};

enum class OpResult : uint8_t {
  // The batch continues to the next operator.
  kContinue,
  // The batch has no rows left; the rest of the pipeline skips it.
  kDrop,
};

// A streaming step in a plan. An operator may add columns or select rows, but
// it always updates one batch in place and produces at most one output batch
// for each input batch.
class Operator {
 public:
  virtual ~Operator();
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  // Made once per execution, before the first batch.
  virtual std::unique_ptr<OperatorState> MakeState() const {
    return std::make_unique<OperatorState>();
  }

  virtual OpResult Execute(RowBatch& batch, OperatorState& state) const = 0;

 protected:
  Operator() = default;
};

// Produces the batches a plan runs over.
class Source {
 public:
  virtual ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // Made once per execution. A source whose input is another source makes
  // that one's state too and holds it, so one call builds a whole chain.
  virtual std::unique_ptr<OperatorState> MakeState() const = 0;

  // Fills `out` with the next batch, or returns false when there are none
  // left. The values it is filled with belong to `state` and are good until
  // the next call, so a caller that wants them for longer copies them.
  virtual bool GetData(RowBatch& out, OperatorState& state) const = 0;

  // Winds `state` back to the first batch so it can be replayed.
  virtual void Rewind(OperatorState& state) const = 0;

  // Distinguishes normal exhaustion from an error after GetData() says there
  // is nothing more.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

 protected:
  Source() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
