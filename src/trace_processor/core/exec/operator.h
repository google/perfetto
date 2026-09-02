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

// The mutable state of one execution of a plan.
//
// Operators and Sources are plan nodes: they stay const while running and hold
// no values. Everything which changes as a query runs lives here, created by
// the plan node and owned by the executor. A plan can be run more than once or
// concurrently, but it and its borrowed dependencies must outlive each run.
class OperatorState {
 public:
  OperatorState() = default;
  virtual ~OperatorState();

  OperatorState(const OperatorState&) = delete;
  OperatorState& operator=(const OperatorState&) = delete;

  // Only ever called on a state this plan node created.
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
  // `out` holds all the output for this input. An empty `out` means the input
  // produced no rows.
  kNeedMoreInput,
  // `out` holds part of the output for this input. Call Execute() again with
  // the same input for the rest.
  kHaveMoreOutput,
  // The operator failed; status() says why.
  kError,
};

// A streaming step in a plan.
//
// One input batch can produce more than one output batch: an operator which
// fans out returns kHaveMoreOutput and is called again with the same input.
// This is why input and output are separate batches: the input has to survive
// being read more than once.
class Operator {
 public:
  virtual ~Operator();
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  virtual std::unique_ptr<OperatorState> MakeState() const {
    return std::make_unique<OperatorState>();
  }

  virtual OpResult Execute(const RowBatch& in,
                           RowBatch& out,
                           OperatorState& state) const = 0;

  // Resets `state` so the plan can be run again. An operator which carries
  // nothing between batches has nothing to do here.
  virtual void Rewind(OperatorState&) const {}

  // Why Execute() returned kError.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

 protected:
  Operator() = default;
};

// Produces the batches a plan runs over.
class Source {
 public:
  virtual ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // A source wrapping another source creates that source's state too, so one
  // call builds the whole chain.
  virtual std::unique_ptr<OperatorState> MakeState() const = 0;

  // Fills `out` and returns true, or returns false when no batches are left.
  // The values in `out` stay valid until the next call.
  virtual bool GetData(RowBatch& out, OperatorState& state) const = 0;

  // Restarts from the first batch.
  virtual void Rewind(OperatorState& state) const = 0;

  // After GetData() returns false, distinguishes exhaustion from failure.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

 protected:
  Source() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
