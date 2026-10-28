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
#include <cstring>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Context;
class RunHeld;

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
};

// The mutable state of one step (Source, Transform or Operator) for one run
// of a plan. Every step keeps to these rules, so that what a step holds, and
// for how long, is decided in one place:
//
//  1. A plan is const once lowering ends: Sources, Transforms and Operators
//     hold no values, and nothing the planner sets changes during a run.
//  2. Whatever a step needs only during one call is not kept past it.
//  3. Whatever a step keeps across calls is either set when its state is
//     made and never changed, scratch for one call kept for its capacity
//     (CallScratch<T>), or held for the run (Held<T> or RunValue<T>, see
//     run_held.h). The executor ends the run of everything
//     held when the step's stream ends: the step has served its last batch,
//     or no more will be pulled from it, and again before a source is
//     restarted (see Restart()). A step never clears its own state; a source
//     only restarts its read position in Rewind().
//  4. A batch a step outputs never points into its state: its columns are in
//     buffers from the run's Context, or borrow storage which outlives the
//     run (see RowBatch). So a state's run can end while batches it output
//     are still being read.
//  5. Batches are not changed once output. A step changing one works on its
//     own RowBatch, onto the same buffers.
//  6. Rows read after the step which produced them has finished are held by
//     the run, never in a step's state.
//
// A plan can be run more than once or concurrently, but it and its borrowed
// dependencies must outlive each run.
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

  // Ends the run of everything the state holds (see RunHeld). Called by the
  // executor, never by the step.
  void EndRun() {
    if (held_) {
      EndHeldRuns();
    }
  }

  // Whether the state holds anything for a run, which is settled once it is
  // made.
  bool holds() const { return held_ != nullptr; }

 private:
  friend class Pipeline;
  friend class RunHeld;
  template <typename T>
  friend class RunValue;
  template <typename T>
  friend class Held;

  // A plain value held for a run: where it is, and the bytes it starts each
  // run with. Put back with a store, so ending a run calls nothing for it.
  struct HeldValue {
    void* where;
    uint64_t initial;
    uint32_t size;
  };
  // Everything the state holds for a run. Behind a pointer, made on the
  // first registration, so a state holding nothing stays small.
  struct Registry {
    std::vector<RunHeld*> held;
    std::vector<HeldValue> values;
    std::vector<base::Status*> statuses;
  };

  Registry& registry() {
    if (!held_) {
      held_ = std::make_unique<Registry>();
    }
    return *held_;
  }
  void EndHeldRuns();
  static void EndRuns(const Registry& registry);

  std::unique_ptr<Registry> held_;
};

// Something a step's state holds for one run. It registers itself with the
// state when made, so the executor can end its run without the step's help.
class RunHeld {
 public:
  RunHeld(const RunHeld&) = delete;
  RunHeld& operator=(const RunHeld&) = delete;

  // Lets go of what the run left, keeping only small allocations for the
  // next run.
  virtual void EndRun() = 0;

 protected:
  explicit RunHeld(OperatorState* owner) {
    owner->registry().held.push_back(this);
  }
  virtual ~RunHeld();
};

inline void OperatorState::EndRuns(const Registry& registry) {
  for (const HeldValue& value : registry.values) {
    switch (value.size) {
      case 1:
        memcpy(value.where, &value.initial, 1);
        break;
      case 4:
        memcpy(value.where, &value.initial, 4);
        break;
      default:
        memcpy(value.where, &value.initial, 8);
        break;
    }
  }
  for (base::Status* status : registry.statuses) {
    if (!status->ok()) {
      *status = base::OkStatus();
    }
  }
  for (RunHeld* held : registry.held) {
    held->EndRun();
  }
}

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

// Ends the run of `state` (see RunHeld) and starts `source` again from its
// first batch. How every consumer restarts a source, so that nothing the last
// run held survives into the next.
inline void Restart(const Source& source, OperatorState& state) {
  state.EndRun();
  source.Rewind(state);
}

// A step changing the batch it is given in place. Unlike an Operator it never
// holds rows back, so a pipeline runs it with nothing copied or buffered.
class Transform {
 public:
  virtual ~Transform();
  Transform(const Transform&) = delete;
  Transform& operator=(const Transform&) = delete;

  // How the step uses the batch's columns (see StepColumns). A Transform keeps
  // every column (StepColumns::Keeps::kAll).
  virtual void DescribeColumns(StepColumns* out) const = 0;

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

  // How the step uses the batches' columns (see StepColumns).
  virtual void DescribeColumns(StepColumns* out) const = 0;

  // Tells an operator which holds rows which columns of its input no later
  // step reads, by column, so it need not hold them. Called by the planner
  // before the plan runs; an operator holding nothing ignores it.
  virtual void SetUnreadColumns(std::vector<bool>) {}

  virtual std::unique_ptr<OperatorState> MakeState(Context&) const {
    return std::make_unique<OperatorState>();
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
