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
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/arena.h"

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

  using Step = std::variant<const Transform*, const Operator*>;

  // Copies `steps` into `arena`.
  Pipeline(core::Arena& arena, const Source&, base::Span<const Step> steps);
  Pipeline(core::Arena&, Source&&, base::Span<const Step>) = delete;

  RowBatch* Next(RowBatch& scratch, OperatorState& state) const override;

  // As Next(): a pipeline outputs batches it holds, so needs no scratch.
  RowBatch* Next(OperatorState& state) const;
  // Ends the run of every segment not yet ended. Used by Context when a side
  // pipeline is stopped before exhaustion.
  void EndRun(OperatorState& state) const;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  // The last step's rows in place, if it is an operator serving them so.
  const RowsInPlace* InPlace(const OperatorState& state) const override;
  bool HasInPlace() const override {
    const Segment& last = segments_[last_segment_];
    if (last.first != last.end) {
      return false;
    }
    return last.start_op != nullptr || source_.HasInPlace();
  }

  // The pipeline's input.
  const Source& input() const { return source_; }

  // The operators and transforms, in the order they run.
  base::Span<const Step> steps() const { return steps_; }

 private:
  friend class PipelineFormatter;

  struct Segment {
    // The step starting the segment, or -1 for the source.
    int32_t start = -1;
    const Operator* start_op = nullptr;
    // Its transforms: the steps [first, end).
    uint32_t first = 0;
    uint32_t end = 0;
  };

  struct State : OperatorState {
    // What a run has run into, and what the plan is, in one byte.
    enum Flag : uint8_t {
      kStopped = 1,
      // Set once a step has failed. The Context holds the reason.
      kFailed = 2,
      kSourceDone = 4,
      // The source's last batch has been served: the next pull ends the run.
      kLastServed = 16,
      // The rest are the plan's, set again when a run starts.
      // Operators follow the source.
      kSegmented = 8,
    };
    // Reasons for Next() to take its slow path.
    static constexpr uint8_t kNextSlow = kStopped | kFailed;
    // Reasons to take the source's segment off its fast path.
    static constexpr uint8_t kAttention = kStopped | kFailed | kSourceDone;

    struct Link {
      const Transform* op;
      OperatorState* state;
    };

    // The segments starting with an operator, by segment.
    struct Start {
      core::ArenaSpan<Link> links;
      // The states whose runs end with this segment's: first the source's or
      // the operator's, then the transforms'.
      core::ArenaSpan<OperatorState*> states;
      RowBatch* input = nullptr;
      // Kept across continuations.
      RowBatch* current = nullptr;
      RowBatch* out = nullptr;
      bool continuation = false;
      bool input_done = false;
      bool finished = false;
      // The run this was last used in; reset on first use in a new one.
      uint64_t run = 0;
    };

    // What Next() reads on its fast path, together.
    uint8_t flags = 0;
    const Source* source = nullptr;
    OperatorState* source_state = nullptr;
    // The last segment's transforms, whichever segment that is.
    core::ArenaSpan<Link> output_links;

    // Each step's state, by step, made once.
    core::ArenaSpan<OperatorState*> operators;
    // One per segment. The first, the source's, uses only its links and states.
    core::ArenaSpan<Start> starts;
    uint64_t run = 0;
    // How many segments, from the first, have had their runs ended.
    uint32_t ended_segments = 0;

    // The batch returned to the caller.
    RowBatch* output = nullptr;
  };

  OperatorState* NewState(Context&) const override;
  // Makes every step's state, and points the links at them.
  void MakeStepStates(State&) const;

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
  // The source has no batches left: records why, and ends its run and those
  // of the first segment's transforms.
  void SourceDone(State&) const;
  // `batch`, about to be served, is the source's last.
  static void LastServed(State&, RowBatch& batch);
  // Nothing more comes out of `segment`, nor of any segment before it: ends
  // the run of the source and of every step up to its end, once a run.
  PERFETTO_ALWAYS_INLINE void EndUpTo(uint32_t segment, State& s) const {
    if (s.ended_segments > segment) {
      return;
    }
    if (!s.context().holding()) {
      s.ended_segments = segment + 1;
      return;
    }
    EndRunsUpTo(segment, s);
  }
  void EndRunsUpTo(uint32_t segment, State&) const;
  // Called when a step fails. Stops the run and makes sure the Context records
  // why.
  static void StepFailed(State&);

  // The plan.
  const Source& source_;
  core::ArenaSpan<Step> steps_;
  core::ArenaSpan<Segment> segments_;
  uint32_t last_segment_ = 0;

  // How it runs: the options, and the State::Flags every run starts with.
  uint8_t plan_flags_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
