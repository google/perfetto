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

// Pulls a source through a chain of operators.
//
// The chain is cut into segments at every operator which is not a Transform.
// A segment's batches come from the source, or from the operator starting it,
// and pass through the segment's transforms in place, so nothing is copied or
// buffered between them. Only the operators starting segments keep their
// input across continuations, and can have small batches combined for them.
// Finish runs only after successful exhaustion. Errors and cancellation
// discard pending output. Rewind resets execution while retained batches stay
// valid. The source and operators must outlive their execution states.
class Pipeline final : public Source {
 public:
  Pipeline(const Source&,
           std::vector<std::unique_ptr<Operator>>,
           ExecutionOptions);
  Pipeline(Source&&,
           std::vector<std::unique_ptr<Operator>>,
           ExecutionOptions) = delete;
  ~Pipeline() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  RowBatch* Next(RowBatch& scratch, OperatorState& state) const override;
  RowBatch* Open(RowBatch& scratch, OperatorState& state) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  struct Segment {
    // The operator starting the segment, or -1 for the source.
    int32_t start = -1;
    // By transform, its operator index and itself.
    std::vector<uint32_t> transforms;
    std::vector<const Transform*> transform_ops;
  };

  // Small batches from a segment, combined for whatever reads them.
  struct Combiner {
    RowBatch scratch;
    // A batch read ahead which did not fit, served next.
    RowBatch* deferred = nullptr;
    BatchBuffer buffered;
    RowBatch combined;
  };

  struct State : OperatorState {
    ~State() override;
    // A fused pipeline's transforms with their states, in order.
    struct Link {
      Transform::ProcessFn process;
      const Transform* op;
      OperatorState* state;
    };
    // What a fused run reads, first so it shares as few lines as it can.
    std::vector<Link> chain;
    std::unique_ptr<OperatorState> source;
    base::Status status = base::OkStatus();
    bool stopped = false;
    bool source_done = false;
    // The source is opened by the first fetch of a run, rather than rewound
    // and then fetched from.
    bool source_open = false;
    std::vector<std::unique_ptr<OperatorState>> operators;
    struct Start {
      Combiner input;
      // The input being worked through, kept across continuations.
      RowBatch* current = nullptr;
      RowBatch out;
      bool continuation = false;
      bool input_done = false;
      bool finished = false;
      // The run this was last used in: one from before is reset on first use,
      // so a rewind need not visit every start.
      uint64_t run = 0;
    };
    // By segment; the first's is unused.
    std::vector<Start> starts;
    Combiner output;
    uint64_t emitted = 0;
    uint64_t run = 0;
  };

  // Runs a fused pipeline's transforms over `batch` and those after it,
  // until one keeps rows.
  RowBatch* Chain(RowBatch* batch, RowBatch& scratch, State&) const;
  void ChainFailed(const Transform&, OperatorState&, State&) const;
  static void ResetStatus(State&);
  // The next non-empty batch of `segment`, after its transforms.
  RowBatch* SegmentNext(uint32_t segment, RowBatch& scratch, State&) const;
  // The next batch of the operator starting `segment`.
  RowBatch* StartNext(uint32_t segment, State&) const;
  // The next batch of `segment`, combined for a reader preferring throughput.
  RowBatch* Read(uint32_t segment, BatchPreference, Combiner&, State&) const;
  bool CanContinue(State&) const;
  void Stop(State&) const;

  // What a fused run reads first, so it shares as few lines as it can.
  const Source& source_;
  // The source and transforms only, with no limit, cancellation or combining:
  // a run is pulling the source through the transforms, nothing to track.
  bool fused_ = false;
  // Settled when the plan is built, so a run does not decide them again.
  bool has_limit_ = false;
  bool has_cancel_ = false;
  bool combine_output_ = false;
  // The operators whose Rewind() does something.
  std::vector<uint32_t> rewound_;
  std::vector<std::unique_ptr<Operator>> operators_;
  ExecutionOptions options_;
  std::vector<Segment> segments_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
