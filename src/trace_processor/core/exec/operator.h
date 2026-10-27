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
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// A plan is a Source followed by steps: Transforms and Operators. Every step
// keeps to these rules, which are what make the plan's memory and the
// planner's passes over it predictable:
//
//  1. A step reads only the columns it describes in DescribeColumns(), and
//     produces the columns it describes there, nothing else.
//  2. A batch it is given lives for one call. A step keeps nothing that
//     points into it afterwards: no batch, view or buffer of it.
//  3. What a step keeps across calls it derived itself: keys, numbers,
//     hash tables, accumulated values, or buffers it filled. Never rows.
//     Rows which must outlive a call are parked by the planner in one of the
//     run's stores (see ParkRows), and a step which reorders rows orders
//     their numbers in the store alone (StepColumns::Keeps::kRowNumbers).
//
// TODO(lalitm): GraphTree and the interval operators still hold rows of their
// inputs in a RowStore of their own, against rule 3.

// How a step uses the columns of the batches it is given: what every
// Transform and Operator describes in DescribeColumns(). The planner's passes
// read and renumber steps through this alone.
struct StepColumns {
  // What a step's output keeps of its input's columns.
  enum class Keeps : uint8_t {
    // Every column, where it was, followed by `added` new ones. Rows may be
    // dropped.
    kAll,
    // None: the output is `added` new columns.
    kNone,
    // A reorder. The planner plans it as keeping every column, but it runs on
    // rows parked in a store (see ParkRows): its input holds their numbers in
    // the store at `numbers`, and its output is those numbers in the new
    // order, every row kept, followed by `added` new columns. The planner parks
    // rows before it
    // and fetches them back after it (see ParkReorderedRows).
    kRowNumbers,
  };

  Keeps keeps = Keeps::kAll;
  // The columns the step adds.
  uint32_t added = 0;
  // Every batch column the step reads, as references to every copy of its
  // position the step keeps, so that a pass can renumber them all.
  std::vector<uint32_t*> reads;
  // For kRowNumbers: the position of the rows' numbers in the input.
  uint32_t* numbers = nullptr;
};

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
// holds rows back, so a pipeline runs it with nothing copied or buffered. It
// keeps every column of the batch (StepColumns::Keeps::kAll).
class Transform {
 public:
  virtual ~Transform();
  Transform(const Transform&) = delete;
  Transform& operator=(const Transform&) = delete;

  // How the step uses the batch's columns (see StepColumns). Not const, as the
  // positions it hands out can be renumbered before the plan runs.
  virtual void DescribeColumns(StepColumns* out) = 0;

  virtual std::unique_ptr<OperatorState> MakeState(Context&) const {
    return std::make_unique<OperatorState>();
  }

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
  // `out` holds the last of the operator's output. It takes no more input,
  // Finish() is not called, and the steps before it are not pulled again.
  kFinished,
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

  // How the step uses the batches' columns (see StepColumns). Not const, as
  // the positions it hands out can be renumbered before the plan runs.
  virtual void DescribeColumns(StepColumns* out) = 0;

  virtual OpResult Execute(const RowBatch& in,
                           RowBatch& out,
                           OperatorState& state) const = 0;

  // Called once after the last input batch, for an operator which holds its
  // output back to let it go. The results mean what they do for Execute(): keep
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
