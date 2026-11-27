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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Breaker;

// Consumes every batch of the pipeline it ends, building what the step
// reading it gets once it is finalized, such as the rows a sort holds or a
// join's hash table of its build side.
class Sink {
 public:
  virtual ~Sink();
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  virtual std::unique_ptr<OperatorState> MakeSinkState(Context&) const = 0;
  virtual base::Status ConsumeBatch(const RowBatch& in,
                                    OperatorState& state) const = 0;

  // Called once, after the last batch.
  virtual base::Status FinalizeSink(OperatorState& state) const = 0;

  // Whether this sink is a Breaker whose output is served by the next pipeline.
  virtual bool IsBreaker() const { return false; }

  // Identifies the concrete class. Each class has a `static constexpr char
  // kType` and returns its address.
  virtual const void* type() const = 0;

  // Whether this is a T. If so, it can be static_cast to T.
  template <typename T>
  bool Is() const {
    return type() == &T::kType;
  }

 protected:
  Sink() = default;
};

// A plan's pipelines besides the one whose output is the plan's. Each ends in
// a sink, built in full before the step reading it reads it, or is streamed
// into the step reading it. A step reads one only through the run's Context
// (see PipelineUse).
class Pipelines {
 public:
  // `input` is what `pipeline` reads, if this owns it, so is declared first
  // to be destroyed last. `sink` is null for a pipeline streamed.
  struct Entry {
    std::unique_ptr<Source> input;
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<Sink> sink;
  };

  Pipelines();
  ~Pipelines();
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;

  // Adds `pipeline`, which reads `input`, ending in `sink` or streamed if
  // null, and returns its id.
  uint32_t Add(std::unique_ptr<Source> input,
               std::unique_ptr<Pipeline> pipeline,
               std::unique_ptr<Sink> sink);

  const Entry& operator[](uint32_t id) const { return entries_[id]; }
  uint32_t size() const { return static_cast<uint32_t>(entries_.size()); }

 private:
  std::vector<Entry> entries_;
};

// A step's use of one of the plan's Pipelines in one run, through the run's
// Context: what its sink built, or its batches as they stream in. The run of
// what it uses ends with the step's (see Held).
class PipelineUse final {
 public:
  PipelineUse(OperatorState* owner, uint32_t id);
  PipelineUse(const PipelineUse&) = delete;
  PipelineUse& operator=(const PipelineUse&) = delete;

  // What the pipeline's sink built, built first if it is not yet.
  base::StatusOr<OperatorState*> Built(Context& context);

  // What the pipeline's sink built, or null if it is not built.
  OperatorState* IfBuilt() const;

  // The pipeline's next batch, or null at its end or on failure, which
  // status() then says.
  RowBatch* Next(Context& context);
  base::Status status() const;

  void EndRun();

 private:
  static void EndRun(void* held) { static_cast<PipelineUse*>(held)->EndRun(); }

  uint32_t id_;
  Context* context_ = nullptr;
};

// The source of the pipeline after a breaker, which serves what the breaker
// built as the sink of the pipeline before.
class Served final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // `breaker` ends pipeline `id`, and must outlive this.
  Served(uint32_t id, const Breaker& breaker);
  ~Served() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState&) const override {}
  base::Status status(const OperatorState& state) const override;
  const RowsInPlace* InPlace(const OperatorState& state) const override;
  bool HasInPlace() const override { return true; }

  uint32_t id() const { return id_; }
  const Breaker& breaker() const { return breaker_; }

 private:
  struct State;

  uint32_t id_;
  const Breaker& breaker_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_
