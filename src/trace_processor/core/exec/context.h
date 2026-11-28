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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

class OperatorState;
class Pipelines;
class RowBatch;
struct ParamValue;

// What one execution of a plan shares while it runs: the storage for the
// batches' own columns, the states of its steps, and the first error.
//
// A step takes a buffer for each column it fills, sized for the column's
// values, and hands it to the batch. Once no batch holds the buffer, it can be
// taken again (see BufferPool).
//
// The context makes every state (Make()) and frees it along with itself.
// EndRun() ends what a state holds for a run (see OperatorState).
//
// Must outlive every batch holding one of its buffers.
class Context {
 public:
  Context();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // A buffer no batch holds, for values `width` bytes wide (see ValueWidth),
  // holding whatever it last held.
  ColumnBuffer TakeBuffer(uint32_t width) { return buffers_.Take(width); }

  // The bytes of value storage taken for buffers so far, held or free.
  uint64_t allocated_buffer_bytes() const { return buffers_.allocated_bytes(); }

  // A shared buffer for columns with no values: every row is null and every
  // value is zero.
  const ColumnBuffer& NullBuffer();

  // The next run's params: the values its operators compare with, already
  // cast (see ParamValue). Nothing is copied: the caller keeps them alive and
  // unchanged until the run ends.
  void SetParams(base::Span<const ParamValue> params) { params_ = params; }
  base::Span<const ParamValue> params() const { return params_; }

  // The plan's pipelines besides its main one, which its steps read by id
  // (see Pipelines). Set before any state of the plan is made.
  void SetPipelines(const Pipelines* pipelines);

  // What pipeline `id`'s sink built: built by running the pipeline to its end
  // the first time it is asked for in a run, or null on failure (see error()).
  // When to build is decided here alone, so an executor could build sinks first
  // instead.
  OperatorState* BuildSink(uint32_t id);

  // What pipeline `id`'s sink built, or null if it is not built in this run.
  OperatorState* IfBuilt(uint32_t id) const;

  // The next batch of pipeline `id`, streamed, started the first time it is
  // asked for in a run. Null at its end or on failure (see error()).
  RowBatch* NextBatch(uint32_t id);

  // Ends the run of pipeline `id` and of what its sink built, so the next asks
  // for them afresh.
  void EndPipelineRun(uint32_t id);

  // Records why the run failed. Only the first failure is kept.
  void Fail(base::Status status);
  // The run's first failure, or ok.
  const base::Status& error() const { return error_; }
  bool failed() const { return !error_.ok(); }
  // Clears the failure, as is done when a run starts.
  void ClearError() {
    if (!error_.ok()) {
      error_ = base::OkStatus();
    }
  }

  // Makes a step's state. The state lives as long as the context. Every state
  // is made here, in a step's NewState().
  template <typename T, typename... Args>
  T* Make(Args&&... args) {
    return MakeState<T>(std::forward<Args>(args)...);
  }

  // Called once a state's NewState() returns.
  OperatorState* Made(OperatorState* state);

  // Ends the run of `state`. This costs only a check when the state holds
  // nothing for the run. Defined in operator.h.
  inline void EndRun(OperatorState& state);

  // Whether any state has something to end when its run ends. If none does,
  // ending runs can be skipped entirely.
  bool holding() const { return holding_ != 0; }

 private:
  // A pipeline's states, made the first time it runs and kept for the next
  // runs, which end them instead.
  struct Slot {
    OperatorState* pipeline = nullptr;
    OperatorState* state = nullptr;
    bool running = false;
    bool built = false;
  };

  struct Owned {
    void* object;
    void (*destroy)(void*);
  };

  // Temporary: destroys a state which is not yet plain data.
  template <typename T>
  static void Destroy(void* object) {
    static_cast<T*>(object)->~T();
  }

  template <typename T, typename... Args>
  T* MakeState(Args&&... args) {
    static_assert(std::is_base_of_v<OperatorState, T>);
    T* state = new (states_.Allocate(sizeof(T), alignof(T)))
        T(std::forward<Args>(args)...);
    if constexpr (!std::is_trivially_destructible_v<T>) {
      AddOwned(state, &Destroy<T>);
    }
    AddState(state);
    return state;
  }

  // Starts a run of pipeline `id`.
  Slot& Start(uint32_t id);

  void AddState(OperatorState* state);
  void AddOwned(void* object, void (*destroy)(void*));
  // What EndRun() does for a state which holds something for the run.
  void EndHeldRun(OperatorState& state);
  void SetHolds(OperatorState& state, bool holds);

  // Declared before everything holding its buffers, so it outlives them.
  BufferPool buffers_;
  ColumnBuffer nulls_;
  base::Span<const ParamValue> params_;
  const Pipelines* pipelines_ = nullptr;
  // By pipeline, sized once, as running a pipeline can run another within it.
  std::unique_ptr<Slot[]> slots_;
  uint32_t slot_count_ = 0;
  base::Status error_;
  // The states, by id.
  core::Arena states_;
  std::vector<OperatorState*> held_;
  // How many states hold something (see OperatorState::holds_).
  uint32_t holding_ = 0;

  std::vector<Owned> owned_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
