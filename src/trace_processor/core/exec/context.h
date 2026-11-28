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

// Run memory of up to this many bytes is kept across runs, so a pipeline run
// over and over on small inputs allocates nothing after its first run.
// Anything larger is given back as soon as the run ends.
inline constexpr uint64_t kRunKeptBytes = 64 * 1024;

// Where a RunVector's values are (see run_vector.h).
struct RunHeader {
  void* data;
  // 64-bit, so that a store to a value never aliases a nearby 32-bit member,
  // which the compiler would otherwise have to reload.
  uint64_t size;
  uint64_t capacity;
};

// What ending its owner's run does to a RunVector.
enum class RunKeep : uint8_t {
  // Empties it, keeping its memory only if it is no bigger than kRunKeptBytes.
  kSmall,
  // Keeps its memory and size. For a hash table's slots, or for a list of
  // RunVectors which are themselves kept for the next run.
  kAll,
  // Like kAll, but with every value zeroed.
  kAllZeroed,
};

// What one execution of a plan shares while it runs: the storage for the
// batches' own columns, the states of its steps and what they hold, and the
// first error.
//
// A step takes a buffer for each column it fills, sized for the column's
// values, and hands it to the batch. Once no batch holds the buffer, it can be
// taken again (see BufferPool).
//
// States are plain data which the context makes (Make()) and frees along with
// itself. Whatever a state holds beyond plain values also belongs to the
// context: the blocks its RunVectors grow into, and the batches and buffers it
// takes. EndRun() restores a state to how it was made and takes all of those
// back (see OperatorState).
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

  // Records that `owner` uses pipeline `id`, so ending `owner`'s run ends
  // pipeline `id`'s run too.
  void UsePipeline(OperatorState& owner, uint32_t id);

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

  // Makes a step's state. The state lives as long as the context, and its
  // values are restored to how they were made at the end of each run. Every
  // state is made here, or by MakeRewound(), in a step's NewState().
  template <typename T, typename... Args>
  T* Make(Args&&... args) {
    return MakeState<T>(true, std::forward<Args>(args)...);
  }
  // Like Make(), but for a state whose step resets in Rewind() every value a
  // run changes, such as a scan's read position. Ending a run leaves its values
  // alone.
  template <typename T, typename... Args>
  T* MakeRewound(Args&&... args) {
    return MakeState<T>(false, std::forward<Args>(args)...);
  }

  // Makes `size` value-initialized Ts for `owner`. Like the state's own values,
  // they are restored at the end of each run to how they were when the state
  // was made. For what a state holds per column, aggregate or step of the plan.
  template <typename T>
  core::ArenaSpan<T> NewRunSpan(OperatorState& owner, uint32_t size) {
    static_assert(std::is_trivially_copyable_v<T> &&
                  std::is_trivially_destructible_v<T>);
    if (size == 0) {
      return {};
    }
    uint64_t bytes = uint64_t{sizeof(T)} * size;
    PERFETTO_CHECK(bytes <= UINT32_MAX);
    core::ArenaSpan<T> span(
        static_cast<T*>(AllocateSpan(static_cast<size_t>(bytes), alignof(T))),
        size);
    AddRunBytes(owner, span.begin(), static_cast<uint32_t>(bytes));
    for (T& value : span) {
      new (&value) T();
    }
    return span;
  }

  // Allocates `size` uninitialized Ts which live as long as the state. Ending a
  // run leaves them alone, so they suit values set when the state is made, or
  // scratch for one call.
  template <typename T>
  core::ArenaSpan<T> NewSpan(uint32_t size) {
    static_assert(
        std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
        "a state is plain data");
    return {static_cast<T*>(AllocateSpan(sizeof(T) * size, alignof(T))), size};
  }
  // Like NewSpan(), but makes a single T from `args`.
  template <typename T, typename... Args>
  T* New(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>, "a state is plain data");
    return new (AllocateSpan(sizeof(T), alignof(T)))
        T(std::forward<Args>(args)...);
  }

  // Makes an object for a state which is not plain data, such as a compiled
  // pattern. It is destroyed along with the context.
  template <typename T, typename... Args>
  T* Own(Args&&... args) {
    T* object = new T(std::forward<Args>(args)...);
    AddOwned(object, &Delete<T>);
    return object;
  }

  // Called once a state's NewState() returns. The state's values at this point
  // are what each of its runs restores.
  OperatorState* Made(OperatorState* state);

  // Ends the run of `state`. Its values, and those of its run spans, are
  // restored to how they were made, unless it was made by MakeRewound(). Each
  // of its RunVectors ends as its RunKeep says; memory which is not kept in
  // place is saved for the state's next run if it is no bigger than
  // kRunKeptBytes. Batches and buffers made with the state are emptied, and
  // those it took are emptied and kept for it to take again. This costs only a
  // check when the state holds nothing for the run. Defined in operator.h.
  inline void EndRun(OperatorState& state);

  // Whether any state has something to end when its run ends. If none does,
  // ending runs can be skipped entirely.
  bool holding() const { return holding_ != 0; }

  // Grows the block of RunVector `header`, owned by `owner` and holding values
  // `width` bytes wide, to hold at least `min_capacity`. One function serves
  // every RunVector type.
  void GrowRun(OperatorState& owner,
               RunHeader* header,
               uint64_t min_capacity,
               uint32_t width,
               RunKeep keep);

  // Makes a batch which lives as long as the state. The context never empties
  // it; whoever holds it does.
  RowBatch* NewBatch();
  // Makes a batch for `owner` which is emptied at the end of each of its runs.
  RowBatch* NewBatch(OperatorState& owner);
  // Takes a batch for one run of `owner`. It is emptied and given back when the
  // run ends.
  RowBatch* TakeBatch(OperatorState& owner);
  // Makes `count` empty buffer handles which live as long as the state. Whoever
  // holds them releases them.
  ColumnBuffer* NewBuffers(uint32_t count);
  // Makes `count` empty buffer handles for `owner`, released at the end of each
  // of its runs.
  ColumnBuffer* NewBuffers(OperatorState& owner, uint32_t count);
  // Takes `count` empty buffer handles for one run of `owner`, released when
  // the run ends.
  ColumnBuffer* TakeBuffers(OperatorState& owner, uint32_t count);

 private:
  // A pipeline's states, made the first time it runs and kept for the next
  // runs, which end them instead.
  struct Slot {
    OperatorState* pipeline = nullptr;
    OperatorState* state = nullptr;
    bool running = false;
    bool built = false;
  };

  // What each state holds, by state id.
  struct Held;

  struct Owned {
    void* object;
    void (*destroy)(void*);
  };

  template <typename T>
  static void Delete(void* object) {
    delete static_cast<T*>(object);
  }

  template <typename T, typename... Args>
  T* MakeState(bool restored, Args&&... args) {
    static_assert(std::is_base_of_v<OperatorState, T>);
    static_assert(std::is_trivially_destructible_v<T>,
                  "a state is plain data: what it holds is the context's");
    T* state = new (states_.Allocate(sizeof(T), alignof(T)))
        T(std::forward<Args>(args)...);
    AddState(state, sizeof(T), restored);
    return state;
  }

  // Starts a run of pipeline `id`.
  Slot& Start(uint32_t id);

  void AddState(OperatorState* state, uint32_t size, bool restored);
  // Memory for a span made with a state. A span of at least a cache line is
  // aligned to whole cache lines, as the vectors these spans replace were.
  void* AllocateSpan(size_t size, size_t align);
  void AddRunBytes(OperatorState& owner, void* where, uint32_t size);
  void AddOwned(void* object, void (*destroy)(void*));
  // What EndRun() does for a state which holds something for the run.
  void EndHeldRun(OperatorState& state);
  // What EndHeldRun() does for the blocks of RunVectors, and for those in
  // lists kept whole.
  static void EndBlocks(Held& held);
  static void EndBatches(Held& held);
  static void EndListedBlocks(Held& held, std::vector<uint8_t>* placed);
  // Whether `held` still needs ending at the end of each run, once this run
  // has ended.
  static bool Holds(const Held& held);
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
  // The states, their spans, and the saved values each run starts from.
  core::Arena states_;
  std::vector<Held> held_;
  // How many states hold something (see OperatorState::holds_).
  uint32_t holding_ = 0;

  // Batches and buffers which live as long as the context (see NewBatch() and
  // NewBuffers()).
  std::vector<std::unique_ptr<RowBatch>> batches_;
  std::vector<std::unique_ptr<ColumnBuffer[]>> buffer_arrays_;
  std::vector<Owned> owned_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
