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

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Context;

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

  // A state keeping data across the batches of a run is made with
  // ResetEachRun, and Reset() before every run but the first.
  virtual void Reset() {}
  bool reset_each_run() const { return reset_each_run_; }

 protected:
  struct ResetEachRun {};
  explicit OperatorState(ResetEachRun) : reset_each_run_(true) {}

 private:
  bool reset_each_run_ = false;
};

// The work running a source can need, decided when it is built: a run skips
// the work whose bit is clear, without testing what the plan is.
enum Need : uint64_t {
  // A run changes the state, so it is reset before the next (Rewind).
  kNeedRewind = 1 << 0,
  // A run's rows come in batches pulled from the source. Without it, the
  // source lends one batch before any run, whose columns never change, and
  // finds each run's one row in them, if any (Lend, FindRow).
  kNeedBatches = 1 << 1,
};
// Every need: whatever a source does not rule out.
inline constexpr uint64_t kNeedsAll = ~uint64_t{0};

// Produces the batches a plan runs over.
class Source {
 public:
  virtual ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // A source wrapping another source creates that source's state too, so one
  // call builds the whole chain. Any column a run fills for a batch comes from
  // `context`, which outlives the state.
  virtual std::unique_ptr<OperatorState> MakeState(Context& context) const = 0;

  // What each column of every batch holds, decided when the source is built.
  virtual const Layout& layout() const = 0;

  // Fills `out` and returns true, or returns false when no batches are left.
  // A column it computes is in a buffer from the run's Context; one it
  // borrows is onto storage which outlives the run (see RowBatch). A
  // successful empty batch is not exhaustion.
  virtual bool GetData(RowBatch& out, OperatorState& state) const = 0;

  // The next batch, or null when none are left: `scratch` filled by
  // GetData(), unless the source has a batch of its own. Valid until the next
  // call.
  virtual RowBatch* Next(RowBatch& scratch, OperatorState& state) const {
    return GetData(scratch, state) ? &scratch : nullptr;
  }

  // Restarts from the first batch.
  virtual void Rewind(OperatorState& state) const = 0;

  // After GetData() returns false, distinguishes exhaustion from failure.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

  // What running it needs, as Need bits. Each must follow from how the
  // source was built: claiming less than it needs gives wrong rows, so the
  // default is everything.
  virtual uint64_t needs() const { return kNeedsAll; }

  // Without kNeedBatches: points `out`, before any run, at the columns every
  // run's row is read from. Its rows are read at the row FindRow() gives,
  // not through its selection.
  virtual void Lend(RowBatch&, OperatorState&) const {
    PERFETTO_FATAL("Lend() without needs() ruling out batches");
  }
  // Without kNeedBatches: this run's row, if it has one, with the run's
  // filter values.
  virtual bool FindRow(OperatorState&, uint32_t*) const {
    PERFETTO_FATAL("FindRow() without needs() ruling out batches");
  }

  // Identifies the concrete class. Each class has a `static constexpr char
  // kType` and returns its address.
  virtual const void* type() const = 0;

  // Whether this is a T. If so, it can be static_cast to T.
  template <typename T>
  bool Is() const {
    return type() == &T::kType;
  }

 protected:
  Source() = default;
};

// A step changing the batch it is given in place. Unlike an Operator it never
// holds rows back, so a pipeline runs it with nothing copied or buffered.
class Transform {
 public:
  virtual ~Transform();
  Transform(const Transform&) = delete;
  Transform& operator=(const Transform&) = delete;

  virtual std::unique_ptr<OperatorState> MakeState(Context&) const {
    return std::make_unique<OperatorState>();
  }

  // Changes `layout`, that of the batches it is given, into that of the
  // batches it outputs.
  virtual void OutputLayout(Layout* layout) const = 0;

  // False on failure, with status() saying why.
  virtual bool Process(RowBatch& batch, OperatorState& state) const = 0;

  // Why Process() returned false.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

  // Identifies the concrete class. Each class has a `static constexpr char
  // kType` and returns its address.
  virtual const void* type() const = 0;

  // Whether this is a T. If so, it can be static_cast to T.
  template <typename T>
  bool Is() const {
    return type() == &T::kType;
  }

 protected:
  Transform() = default;
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
// being read more than once. Output can also come after the last input, from
// Finish().
class Operator {
 public:
  virtual ~Operator();
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  virtual std::unique_ptr<OperatorState> MakeState(Context&) const {
    return std::make_unique<OperatorState>();
  }

  // Changes `layout`, that of the batches it is given, into that of the
  // batches it outputs.
  virtual void OutputLayout(Layout* layout) const = 0;

  virtual OpResult Execute(const RowBatch& in,
                           RowBatch& out,
                           OperatorState& state) const = 0;

  // Called once after the last input batch, for an operator which holds rows
  // back to let them go. The results mean what they do for Execute(): keep
  // returning kHaveMoreOutput to be called again. An operator which holds
  // nothing back leaves `out` empty.
  virtual OpResult Finish(RowBatch& out, OperatorState&) const {
    out.Reset();
    return OpResult::kNeedMoreInput;
  }

  // Why Execute() or Finish() returned kError.
  virtual base::Status status(const OperatorState&) const {
    return base::OkStatus();
  }

  // Identifies the concrete class. Each class has a `static constexpr char
  // kType` and returns its address.
  virtual const void* type() const = 0;

  // Whether this is a T. If so, it can be static_cast to T.
  template <typename T>
  bool Is() const {
    return type() == &T::kType;
  }

 protected:
  Operator() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
