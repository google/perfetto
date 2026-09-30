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
#include <functional>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/batch_buffer.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Optional batching policy and execution demand. Finite demand disables
// optional lookahead at every boundary; intrinsic blocking operators still
// consume the input needed to produce their first result. Cancellation is
// checked between source/operator calls, not within individual kernels.
struct ExecutionOptions {
  BatchPreference preference = BatchPreference::kLatency;
  uint64_t limit = std::numeric_limits<uint64_t>::max();
  uint32_t small_batch_rows = 64;
  uint32_t target_batch_rows = kMaxBatchRows;
  std::function<bool()> cancelled;
};

// Pulls a source through a chain of steps. The chain is cut into segments at
// each Operator, and a segment's Transforms run in place on the batches coming
// into it. Only operators retain input across continuations and can have
// small batches combined for them.
// Finish runs only after successful exhaustion. Errors and cancellation
// discard pending output. Rewind resets execution while retained batches stay
// valid. The source and operators must outlive their execution states.
class Pipeline final : public Source {
 public:
  using Step =
      std::variant<std::unique_ptr<Transform>, std::unique_ptr<Operator>>;

  Pipeline(const Source&, std::vector<Step>, ExecutionOptions);
  Pipeline(Source&&, std::vector<Step>, ExecutionOptions) = delete;
  ~Pipeline() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  RowBatch* Next(RowBatch& scratch, OperatorState& state) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  struct Segment {
    // The step starting the segment, or -1 for the source.
    int32_t start = -1;
    const Operator* start_op = nullptr;
    BatchPreference preference = BatchPreference::kLatency;
    std::vector<uint32_t> transforms;
    std::vector<const Transform*> transform_ops;
  };

  // Small batches from a segment, combined for whatever reads them.
  struct Combiner {
    RowBatch scratch;
    // Read ahead but did not fit: served next.
    RowBatch* deferred = nullptr;
    BatchBuffer buffered;
    RowBatch combined;
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
      kCancellable = 8,
      kLimit = 16,
      kCombineOutput = 32,
      // Operators follow the source.
      kSegmented = 64,
    };
    // Reasons for Next() to take its slow path.
    static constexpr uint8_t kNextSlow =
        kStopped | kFailed | kCancellable | kLimit | kCombineOutput;
    // Reasons to take the source's segment off its fast path.
    static constexpr uint8_t kAttention =
        kStopped | kFailed | kSourceDone | kCancellable;
    // Something is held between batches: an operator or combined output.
    static constexpr uint8_t kHolds = kSegmented | kCombineOutput;

    struct Link {
      const Transform* op;
      OperatorState* state;
    };

    // What Next() reads on its fast path, together.
    uint8_t flags = 0;
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
      Combiner input;
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

    // The output, and how much of the limit it has used.
    Combiner output;
    uint64_t emitted = 0;
  };

  // Pulling batches.
  RowBatch* NextSlow(State&) const;
  // The next non-empty batch of `segment`, after its transforms.
  RowBatch* SegmentNext(uint32_t segment, RowBatch& scratch, State&) const;
  RowBatch* PullSlow(uint32_t segment, RowBatch& scratch, State&) const;
  // The next batch of the operator starting `segment`.
  RowBatch* StartNext(uint32_t segment, State&) const;
  // The next batch of `segment`, combined for a reader preferring throughput.
  RowBatch* Read(uint32_t segment, BatchPreference, Combiner&, State&) const;

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
  ExecutionOptions options_;
  uint8_t plan_flags_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
