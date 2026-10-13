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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/batch_buffer.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Pulls batches from a source through a chain of steps. The chain is split
// into segments at each Operator. A segment's Transforms modify the batches
// entering it in place. Only operators keep input between calls.
//
// Finish runs only after the source is exhausted without error. An error
// discards pending output. Rewind restarts execution. Batches already
// returned stay valid after Rewind. The source and operators must outlive
// their execution states.
class Pipeline final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  using Step =
      std::variant<std::unique_ptr<Transform>, std::unique_ptr<Operator>>;

  Pipeline(const Source&, std::vector<Step>);
  Pipeline(Source&&, std::vector<Step>) = delete;
  ~Pipeline() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  RowBatch* Next(RowBatch& scratch, OperatorState& state) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

  // The pipeline's input.
  const Source& input() const { return source_; }

  // The operators and transforms, in the order they run.
  const std::vector<Step>& steps() const { return steps_; }

 private:
  friend class PipelineFormatter;

  struct Segment {
    // The step starting the segment, or -1 for the source.
    int32_t start = -1;
    const Operator* start_op = nullptr;
    std::vector<uint32_t> transforms;
    std::vector<const Transform*> transform_ops;
  };

  struct State : OperatorState {
    ~State() override;

    // What a run has run into, and what the plan is, in one byte.
    enum Flag : uint8_t {
      kStopped = 1,
      // Mirrors !status.ok().
      kFailed = 2,
      kSourceDone = 4,
      // The rest are the plan's, set again when a run starts.
      // Operators follow the source.
      kSegmented = 8,
    };
    // Reasons for Next() to take its slow path.
    static constexpr uint8_t kNextSlow = kStopped | kFailed;
    // Reasons to take the source's segment off its fast path.
    static constexpr uint8_t kAttention = kStopped | kFailed | kSourceDone;
    // An operator may hold rows between batches.
    static constexpr uint8_t kHolds = kSegmented;

    struct Link {
      const Transform* op;
      OperatorState* state;
    };

    // What Next() reads on its fast path, together.
    uint8_t flags = 0;
    Context* context = nullptr;
    const Source* source = nullptr;
    OperatorState* source_state = nullptr;
    // The last segment's transforms, whichever segment that is.
    const std::vector<Link>* output_links = nullptr;
    std::vector<Link> source_links;

    // Why the run failed, while kFailed is set.
    base::Status status = base::OkStatus();

    // The steps' states, by step, and those Rewind() resets.
    std::unique_ptr<OperatorState> owned_source_state;
    std::vector<std::unique_ptr<OperatorState>> operators;
    std::vector<OperatorState*> reset_each_run;

    // The segments starting with an operator, by segment.
    struct Start {
      std::vector<Link> links;
      RowBatch input;
      // Kept across continuations.
      RowBatch* current = nullptr;
      RowBatch out;
      bool continuation = false;
      bool input_done = false;
      bool finished = false;
      // The run this was last used in; reset on first use in a new one.
      uint64_t run = 0;
    };
    std::vector<Start> starts;
    uint64_t run = 0;

    // The batch returned to the caller.
    RowBatch output;
  };

  // Pulling batches.
  RowBatch* NextSlow(State&) const;
  // The next non-empty batch of `segment`, after its transforms.
  RowBatch* SegmentNext(uint32_t segment, RowBatch& scratch, State&) const;
  RowBatch* PullSlow(uint32_t segment, RowBatch& scratch, State&) const;
  // The next batch of the operator starting `segment`.
  RowBatch* StartNext(uint32_t segment, State&) const;

  // Stopping and failing.
  bool CanContinue(State&) const;
  void Stop(State&) const;
  void SourceStatus(State&) const;
  // Records a failure; an ok status records nothing.
  static void Fail(State&, base::Status status);
  static void StepFailed(base::Status status, State&);
  static void TransformFailed(const State::Link& link, State&);

  // The plan.
  const Source& source_;
  std::vector<Step> steps_;
  std::vector<Segment> segments_;
  uint32_t last_segment_ = 0;

  // How it runs: the options, and the State::Flags every run starts with.
  uint8_t plan_flags_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
