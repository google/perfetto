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

#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"

namespace perfetto::trace_processor::core::exec {

struct ParamValue;
class OperatorState;
class RowBatch;
class Pipelines;

// What one execution of a plan shares as it runs: the storage the batches'
// own columns are in. A step takes a buffer for each column it fills, sized
// for the column's values, and hands it to the batch; once no batch holds it,
// the buffer comes back to be taken again (see BufferPool).
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
  // the first time it is asked for in a run. When to build is decided here
  // alone, so an executor could build sinks first instead.
  base::StatusOr<OperatorState*> BuildSink(uint32_t id);

  // What pipeline `id`'s sink built, or null if it is not built in this run.
  OperatorState* IfBuilt(uint32_t id) const;

  // The next batch of pipeline `id`, streamed, started the first time it is
  // asked for in a run. Null at its end or on failure, which
  // PipelineStatus() then says.
  RowBatch* NextBatch(uint32_t id);
  base::Status PipelineStatus(uint32_t id) const;

  // Ends the run of pipeline `id` and of what its sink built (see Held),
  // so the next asks for them afresh.
  void EndPipelineRun(uint32_t id);

 private:
  // A pipeline's states, made the first time it runs and kept for the next
  // runs, which end them instead.
  struct Slot {
    Slot();
    ~Slot();

    std::unique_ptr<OperatorState> pipeline;
    std::unique_ptr<OperatorState> state;
    bool running = false;
    bool built = false;
  };

  // Starts a run of pipeline `id`.
  Slot& Start(uint32_t id);

  // Declared before `nulls_` and the pipelines' states, so it outlives them.
  BufferPool buffers_;
  ColumnBuffer nulls_;
  base::Span<const ParamValue> params_;
  const Pipelines* pipelines_ = nullptr;
  // By pipeline, sized once, as running a pipeline can run another within it.
  std::unique_ptr<Slot[]> slots_;
  uint32_t slot_count_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
