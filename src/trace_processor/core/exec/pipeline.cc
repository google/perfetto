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

#include "src/trace_processor/core/exec/pipeline.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr size_t kTransformStep =
    base::variant_index<Pipeline::Step, const Transform*>();

}  // namespace

Pipeline::Pipeline(core::Arena& arena,
                   const Source& source,
                   base::Span<const Step> steps)
    : source_(source), steps_(arena.CopySpan<Step>(steps)) {
  uint32_t operators = 0;
  for (const Step& step : steps_) {
    operators += step.index() != kTransformStep;
  }
  segments_ = arena.NewSpan<Segment>(operators + 1);
  segments_[0] = {-1, nullptr, 0, 0};
  uint32_t last = 0;
  for (uint32_t i = 0; i < steps_.size(); ++i) {
    if (steps_[i].index() == kTransformStep) {
      segments_[last].end = i + 1;
      continue;
    }
    const Operator* op = base::unchecked_get<const Operator*>(steps_[i]);
    segments_[++last] = {static_cast<int32_t>(i), op, i + 1, i + 1};
  }
  last_segment_ = last;
  if (last > 0) {
    plan_flags_ |= State::kSegmented;
  }
}
OperatorState* Pipeline::NewState(Context& context) const {
  auto* state = context.MakeRewound<State>();
  state->source = &source_;
  state->source_state = source_.MakeState(context);
  state->starts = context.NewSpan<State::Start>(segments_.size());
  state->output = context.NewBatch();
  MakeStepStates(*state);
  state->output_links = state->starts[last_segment_].links;
  state->flags = plan_flags_;
  return state;
}

void Pipeline::MakeStepStates(State& s) const {
  Context& context = s.context();
  s.operators = context.NewSpan<OperatorState*>(steps_.size());
  for (uint32_t i = 0; i < steps_.size(); ++i) {
    const Transform* const* transform =
        std::get_if<const Transform*>(&steps_[i]);
    s.operators[i] =
        transform ? (*transform)->MakeState(context)
                  : base::unchecked_get<const Operator*>(steps_[i])->MakeState(
                        context);
  }
  for (uint32_t seg = 0; seg < segments_.size(); ++seg) {
    const Segment& segment = segments_[seg];
    State::Start& start = s.starts[seg];
    start = State::Start();
    uint32_t transforms = segment.end - segment.first;
    start.links = context.NewSpan<State::Link>(transforms);
    start.states = context.NewSpan<OperatorState*>(transforms + 1);
    start.states[0] = segment.start < 0
                          ? s.source_state
                          : s.operators[static_cast<uint32_t>(segment.start)];
    for (uint32_t t = segment.first; t < segment.end; ++t) {
      start.links[t - segment.first] = {
          base::unchecked_get<const Transform*>(steps_[t]), s.operators[t]};
      start.states[t - segment.first + 1] = s.operators[t];
    }
    if (seg) {
      start.input = context.NewBatch();
      start.out = context.NewBatch();
    }
  }
}

void Pipeline::EndRun(OperatorState& state) const {
  EndUpTo(last_segment_, state.Cast<State>());
}

void Pipeline::Rewind(OperatorState& state) const {
  auto& s = state.Cast<State>();
  ++s.run;
  s.flags = plan_flags_;
  // Whatever the last run left, if it stopped before its streams ended.
  EndUpTo(last_segment_, s);
  s.ended_segments = 0;
  source_.Rewind(*s.source_state);
}

// A batch is one pull, from the source or the operator starting the last
// segment, and that segment's transforms. Limits, combining, cancellation and
// failure are NextSlow()'s.
// Out of line, so GetData calls it rather than holding a copy.
RowBatch* Pipeline::Next(RowBatch&, OperatorState& state) const {
  return Next(state);
}

PERFETTO_NO_INLINE RowBatch* Pipeline::Next(OperatorState& state) const {
  auto& s = state.Cast<State>();
  uint8_t flags = s.flags;
  if (PERFETTO_UNLIKELY(flags & State::kNextSlow)) {
    return NextSlow(s);
  }
  RowBatch* batch;
  if (!(flags & State::kSegmented)) {
    // Holding nothing, the run ends with its source: nothing to stop.
    if (PERFETTO_UNLIKELY(flags & State::kSourceDone)) {
      return nullptr;
    }
    batch = s.source->Next(*s.output, *s.source_state);
    if (PERFETTO_UNLIKELY(!batch)) {
      SourceDone(s);
      return nullptr;
    }
  } else {
    batch = StartNext(last_segment_, s);
    if (PERFETTO_UNLIKELY(!batch)) {
      Stop(s);
      return nullptr;
    }
  }
  for (const State::Link& link : s.output_links) {
    if (PERFETTO_UNLIKELY(!link.op->Process(*batch, *link.state))) {
      TransformFailed(link, s);
      return nullptr;
    }
    // A batch with no rows left has nothing for later transforms to do.
    if (PERFETTO_UNLIKELY(!batch->size())) {
      return NextSlow(s);
    }
  }
  return batch;
}

PERFETTO_NO_INLINE RowBatch* Pipeline::NextSlow(State& s) const {
  auto last = static_cast<uint32_t>(segments_.size() - 1);
  RowBatch* batch = SegmentNext(last, *s.output, s);
  if (!batch) {
    Stop(s);
  }
  return batch;
}

PERFETTO_ALWAYS_INLINE RowBatch* Pipeline::SegmentNext(uint32_t segment,
                                                       RowBatch& scratch,
                                                       State& s) const {
  core::ArenaSpan<State::Link> links = s.starts[segment].links;
  for (;;) {
    RowBatch* batch;
    if (PERFETTO_LIKELY(segment == 0 && !(s.flags & State::kAttention))) {
      batch = source_.Next(scratch, *s.source_state);
      if (PERFETTO_UNLIKELY(!batch)) {
        SourceDone(s);
        return nullptr;
      }
    } else {
      batch = PullSlow(segment, scratch, s);
      if (!batch) {
        return nullptr;
      }
    }
    for (const State::Link& link : links) {
      if (PERFETTO_UNLIKELY(!link.op->Process(*batch, *link.state))) {
        TransformFailed(link, s);
        return nullptr;
      }
      // A batch with no rows left has nothing for later transforms to do.
      if (PERFETTO_UNLIKELY(!batch->size())) {
        break;
      }
    }
    if (PERFETTO_LIKELY(batch->size())) {
      return batch;
    }
  }
}

PERFETTO_NO_INLINE RowBatch* Pipeline::PullSlow(uint32_t segment,
                                                RowBatch& scratch,
                                                State& s) const {
  if (!CanContinue(s)) {
    return nullptr;
  }
  if (segment != 0) {
    return StartNext(segment, s);
  }
  if (s.flags & State::kSourceDone) {
    return nullptr;
  }
  RowBatch* batch = source_.Next(scratch, *s.source_state);
  if (!batch) {
    SourceDone(s);
  }
  return batch;
}

RowBatch* Pipeline::StartNext(uint32_t segment, State& s) const {
  auto& start = s.starts[segment];
  if (start.run != s.run) {
    start.current = nullptr;
    start.continuation = false;
    start.input_done = false;
    start.finished = false;
    start.run = s.run;
  }
  auto index = static_cast<uint32_t>(segments_[segment].start);
  const Operator& op = *segments_[segment].start_op;
  OperatorState& op_state = *s.operators[index];
  for (;;) {
    if (!CanContinue(s) || start.finished) {
      if (start.finished) {
        // Its last batch has been through the segment's transforms by now.
        EndUpTo(segment, s);
      }
      return nullptr;
    }
    bool has_input = start.continuation;
    if (!has_input && !start.input_done) {
      start.current = SegmentNext(segment - 1, *start.input, s);
      if (start.current) {
        has_input = true;
      } else if (s.flags & (State::kStopped | State::kFailed)) {
        return nullptr;
      } else {
        start.input_done = true;
      }
    }
    start.out->Reset();
    OpResult result = has_input
                          ? op.Execute(*start.current, *start.out, op_state)
                          : op.Finish(*start.out, op_state);
    if (result == OpResult::kError) {
      StepFailed(op.status(op_state), s);
      return nullptr;
    }
    start.continuation = has_input && result == OpResult::kHaveMoreOutput;
    if (result == OpResult::kFinished ||
        (!has_input && result != OpResult::kHaveMoreOutput)) {
      start.finished = true;
    }
    if (start.out->size()) {
      return start.out;
    }
    if (start.finished) {
      EndUpTo(segment, s);
      return nullptr;
    }
  }
}

bool Pipeline::GetData(RowBatch& out, OperatorState& state) const {
  // Releases the caller's last batch first, so its storage can be reused.
  out.Reset();
  RowBatch* batch = Next(out, state);
  if (!batch) {
    out.Reset();
    return false;
  }
  if (batch != &out) {
    out.CopyFrom(*batch);
  }
  return true;
}

PERFETTO_ALWAYS_INLINE bool Pipeline::CanContinue(State& state) const {
  return PERFETTO_LIKELY(!(state.flags & (State::kStopped | State::kFailed)));
}

PERFETTO_NO_INLINE void Pipeline::Stop(State& state) const {
  for (State::Start& start : state.starts) {
    start.current = nullptr;
  }
  state.flags |= State::kStopped;
}

PERFETTO_NO_INLINE void Pipeline::SourceDone(State& s) const {
  s.flags |= State::kSourceDone;
  // Read the source's status now, because ending its run resets it.
  Context& context = s.context();
  if (!context.failed()) {
    base::Status status = source_.status(*s.source_state);
    if (!status.ok()) {
      context.Fail(std::move(status));
    }
  }
  if (context.failed()) {
    s.flags |= State::kFailed;
  }
  EndUpTo(0, s);
}

PERFETTO_NO_INLINE void Pipeline::EndRunsUpTo(uint32_t segment,
                                              State& s) const {
  Context& context = s.context();
  for (uint32_t seg = s.ended_segments; seg <= segment; ++seg) {
    for (OperatorState* state : s.starts[seg].states) {
      context.EndRun(*state);
    }
  }
  s.ended_segments = segment + 1;
}

base::Status Pipeline::status(const OperatorState& state) const {
  return state.context().error();
}

void Pipeline::StepFailed(base::Status status, State& state) {
  Context& context = state.context();
  if (!context.failed()) {
    context.Fail(status.ok()
                     ? base::ErrStatus("operator failed without saying why")
                     : std::move(status));
  }
  state.flags |= State::kFailed;
}

PERFETTO_NO_INLINE void Pipeline::TransformFailed(const State::Link& link,
                                                  State& state) {
  StepFailed(link.op->status(*link.state), state);
}

const RowsInPlace* Pipeline::InPlace(const OperatorState& state) const {
  const Segment& last = segments_[last_segment_];
  if (last.first != last.end) {
    return nullptr;
  }
  const auto& s = state.Cast<State>();
  if (!last.start_op) {
    return source_.InPlace(*s.source_state);
  }
  return last.start_op->InPlace(
      *s.operators[static_cast<uint32_t>(last.start)]);
}

}  // namespace perfetto::trace_processor::core::exec
