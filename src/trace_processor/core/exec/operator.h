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

// Where one execution of a plan has got to. A plan holds no values; every
// value a query holds is in some state, owned by whoever is running the plan.
class OperatorState {
 public:
  OperatorState() = default;
  virtual ~OperatorState();

  OperatorState(const OperatorState&) = delete;
  OperatorState& operator=(const OperatorState&) = delete;

  // An operator only ever sees a state it made itself.
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
  // `out` holds everything this input had to give. An empty `out` means the
  // input came to nothing.
  kNeedMoreInput,
  // `out` holds part of what this input has to give; ask again with the same
  // input.
  kHaveMoreOutput,
  // The operator failed; its state says why.
  kError,
};

// A streaming step in a plan.
//
// One input batch may make more than one output batch: an operator which
// fans out says kHaveMoreOutput and is asked again with the same input. That
// is why input and output are separate batches rather than one updated in
// place -- an input has to survive being read more than once.
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

  // Throws away whatever was carried between batches, so the plan can be run
  // again. An operator which answers for one batch at a time has nothing to
  // do here.
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

  // A source whose input is another source makes that one's state too, so
  // one call builds the chain.
  virtual std::unique_ptr<OperatorState> MakeState() const = 0;

  // Fills `out`, or returns false when there are no batches left. What it is
  // filled with is good until the next call.
  virtual bool GetData(RowBatch& out, OperatorState& state) const = 0;

  // Winds back to the first batch.
  virtual void Rewind(OperatorState& state) const = 0;

  // Tells exhaustion from failure after GetData() says there is no more.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

 protected:
  Source() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
