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
#include <vector>

#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

class Context;
struct RowsInPlace;

// How a step uses the columns of the batches it is given: what every
// Transform and Operator describes in DescribeColumns(), so the planner can
// tell which columns are read after each step.
struct StepColumns {
  // What a step's output keeps of its input's columns.
  enum class Keeps : uint8_t {
    // Every column, where it was, followed by `added` new ones.
    kAll,
    // None: the output is `added` new columns.
    kNone,
  };

  Keeps keeps = Keeps::kAll;
  // The columns the step adds.
  uint32_t added = 0;
  // Every column of its input the step reads.
  std::vector<uint32_t> reads;
  // Columns it reads back from rows it holds, so must hold even if unread.
  std::vector<uint32_t> held_reads;
  // The columns of `reads` the step reads as a scan lends them, sparse (see
  // ColumnView::Kind::kSparse), so that they need no Expand before it.
  std::vector<uint32_t> reads_sparse;
  // Whether an operator holds the columns a scan lends sparse as they are, so
  // that only those it reads otherwise need an Expand before it.
  bool keeps_sparse = false;
};

// The mutable state of one step (Source, Transform or Operator) for one run
// of a plan. Every step keeps to these rules, so that what a step holds, and
// for how long, is decided in one place:
//
//  1. A plan is const once lowering ends: Sources, Transforms and Operators
//     hold no values, and nothing the planner sets changes during a run.
//  2. Whatever a step needs only during one call is not kept past it.
//  3. A state is plain data. The run's Context makes it (Context::Make()) in
//     the step's NewState() and never destroys it. Whatever it keeps across
//     calls is set when the state is made and never changed, is scratch for one
//     call (an array from Context::NewSpan(), bounded by kMaxBatchRows), or is
//     a value of the run: a plain value, or one in a span from
//     Context::NewRunSpan(). Anything which grows is a RunVector, whose memory
//     belongs to the Context. Batches and buffers a state keeps also come from
//     the Context.
//  4. The executor ends a state's run when the step's stream ends, meaning the
//     step has served its last batch or no more will be pulled from it, and
//     again before a source is restarted (see Restart()). Ending a run works
//     the same way for every state (Context::EndRun()). The state's values are
//     restored to how they were when NewState() returned, unless the step
//     resets them itself in Rewind() (Context::MakeRewound()). Each RunVector
//     is emptied, keeping its memory only if it is no bigger than
//     kRunKeptBytes, or is kept whole as a hash table keeps its slots (see
//     RunKeep). The batches and buffers the run took are given back. So a step
//     never clears its own state, a source only resets its read position in
//     Rewind(), and a rewound state behaves exactly like a new one. A state
//     which held nothing for the run costs nothing to end.
//  5. A step which fails reports why with Context::Fail(), then returns
//     failure.
//  6. A batch a step outputs never points into its state: its columns are in
//     buffers from the run's Context, or borrow storage which outlives the
//     run (see RowBatch). So a state's run can end while batches it output
//     are still being read.
//  7. Batches are not changed once output. A step changing one works on its
//     own RowBatch, onto the same buffers.
//  8. Rows read after the step which produced them has finished are held by
//     the run, never in a step's state.
//
// A plan can be run more than once or concurrently, but it and its borrowed
// dependencies must outlive each run.
class OperatorState {
 public:
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

  // The run's Context, which made the state.
  Context& context() const { return *context_; }

 protected:
  OperatorState() = default;
  // Never called: a state is plain data, and its Context frees the memory.
  ~OperatorState() = default;

 private:
  friend class Context;

  Context* context_ = nullptr;
  // The index of this state in its Context.
  uint32_t id_ = 0;
  // Whether the Context has anything to do when this state's run ends. It is 32
  // bits wide so that the class has no padding in which a derived state's
  // members could be laid out.
  uint32_t holds_ = 0;
};

inline void Context::EndRun(OperatorState& state) {
  if (state.holds_) {
    EndHeldRun(state);
  }
}

// The state of a step which keeps nothing.
struct EmptyState final : OperatorState {};

// Produces the batches a plan runs over.
class Source {
 public:
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // A source wrapping another source makes that source's state too, so one
  // call builds the whole chain. Any column a run fills for a batch comes from
  // `context`, which outlives the state.
  OperatorState* MakeState(Context& context) const {
    return context.Made(NewState(context));
  }

  // Fills `out` and returns true, or returns false when no batches are left.
  // A column it computes is in a buffer from the run's Context; one it
  // borrows is onto storage which outlives the run (see RowBatch). A
  // successful empty batch is not exhaustion.
  virtual bool GetData(RowBatch& out, OperatorState& state) const = 0;

  // The next batch, or null when none are left: `scratch` filled by
  // GetData(), unless the source has a batch of its own. Valid until the next
  // call.
  virtual RowBatch* Next(RowBatch& scratch, OperatorState& state) const;

  // Restarts from the first batch.
  virtual void Rewind(OperatorState& state) const = 0;

  // Set when the batches served are read in place: each then holds only a
  // row count, its rows being the next of these. Valid until the run ends.
  virtual const RowsInPlace* InPlace(const OperatorState&) const {
    return nullptr;
  }
  // Whether InPlace() can ever return non-null, so readers which cannot need
  // not ask after every batch.
  virtual bool HasInPlace() const { return false; }

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
  // Made in the plan's Arena, which frees memory without calling destructors.
  ~Source() = default;

  // The state, made with Context::Make().
  virtual OperatorState* NewState(Context& context) const = 0;
};

// Ends the run of `state` (see OperatorState), clears any failure, and starts
// `source` again from its first batch. Every consumer restarts a source this
// way, so nothing the last run held survives into the next.
inline void Restart(const Source& source, OperatorState& state) {
  Context& context = state.context();
  context.ClearError();
  context.EndRun(state);
  source.Rewind(state);
}

// A step changing the batch it is given in place. Unlike an Operator it never
// holds rows back, so a pipeline runs it with nothing copied or buffered.
class Transform {
 public:
  Transform(const Transform&) = delete;
  Transform& operator=(const Transform&) = delete;

  // How the step uses the batch's columns (see StepColumns). A Transform keeps
  // every column (StepColumns::Keeps::kAll).
  virtual void DescribeColumns(StepColumns* out) const = 0;

  OperatorState* MakeState(Context& context) const {
    return context.Made(NewState(context));
  }

  // Returns false on failure, after reporting why with Context::Fail().
  virtual bool Process(RowBatch& batch, OperatorState& state) const = 0;

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
  // Made in the plan's Arena, which frees memory without calling destructors.
  ~Transform() = default;

  // The state, made with Context::Make().
  virtual OperatorState* NewState(Context& context) const;
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
  // The operator failed, and reported why with Context::Fail().
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
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  // How the step uses the batches' columns (see StepColumns).
  virtual void DescribeColumns(StepColumns* out) const = 0;

  // Tells an operator which holds rows which columns of its input no later
  // step reads, by column, so it need not hold them. Called by the planner
  // before the plan runs; an operator holding nothing ignores it.
  virtual void SetUnreadColumns(core::ArenaSpan<bool>) {}

  OperatorState* MakeState(Context& context) const {
    return context.Made(NewState(context));
  }

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

  // As Source::InPlace(), for the batches Finish() outputs.
  virtual const RowsInPlace* InPlace(const OperatorState&) const {
    return nullptr;
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
  // Made in the plan's Arena, which frees memory without calling destructors.
  ~Operator() = default;

  // The state, made with Context::Make().
  virtual OperatorState* NewState(Context& context) const;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
