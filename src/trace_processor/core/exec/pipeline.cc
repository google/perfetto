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
#include <memory>
#include <utility>
#include <variant>
#include <vector>

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
Pipeline::State::~State() = default;

std::unique_ptr<OperatorState> Pipeline::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->context = &context;
  state->owned_source_state = source_.MakeState(context);
  state->source = &source_;
  state->source_state = state->owned_source_state.get();
  // Built at its size, the vector needs no code to grow.
  state->starts = std::vector<State::Start>(segments_.size());
  MakeStepStates(*state);
  state->output_links = last_segment_ == 0
                            ? &state->source_links
                            : &state->starts[last_segment_].links;
  state->flags = plan_flags_;
  return state;
}

void Pipeline::MakeStepStates(State& s) const {
  s.operators.reserve(steps_.size());
  for (const Step& step : steps_) {
    const Transform* const* transform = std::get_if<const Transform*>(&step);
    s.operators.push_back(
        transform ? (*transform)->MakeState(*s.context)
                  : base::unchecked_get<const Operator*>(step)->MakeState(
                        *s.context));
  }
  for (uint32_t seg = 0; seg < segments_.size(); ++seg) {
    const Segment& segment = segments_[seg];
    auto& links = seg == 0 ? s.source_links : s.starts[seg].links;
    for (uint32_t t = segment.first; t < segment.end; ++t) {
      links.push_back({base::unchecked_get<const Transform*>(steps_[t]),
                       s.operators[t].get()});
    }
  }
  s.source_holds = s.source_state->holds();
  s.held.resize(segments_.size());
  auto add = [&s](const OperatorState& state, OperatorState::Registry* into) {
    if (const OperatorState::Registry* r = state.held_.get()) {
      into->held.insert(into->held.end(), r->held.begin(), r->held.end());
      into->values.insert(into->values.end(), r->values.begin(),
                          r->values.end());
      into->statuses.insert(into->statuses.end(), r->statuses.begin(),
                            r->statuses.end());
      s.holds_any = true;
    }
  };
  for (uint32_t seg = 0; seg < segments_.size(); ++seg) {
    const Segment& segment = segments_[seg];
    if (segment.start < 0) {
      add(*s.source_state, &s.held[seg]);
    }
    for (auto t = static_cast<uint32_t>(std::max(segment.start, 0));
         t < segment.end; ++t) {
      add(*s.operators[t], &s.held[seg]);
    }
  }
  if (s.holds_any) {
    s.AddHeld(&s, &Pipeline::EndStateRun);
  }
}

void Pipeline::Rewind(OperatorState& state) const {
  auto& s = state.Cast<State>();
  ++s.run;
  if (s.flags & State::kFailed) {
    s.status = base::OkStatus();
  }
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
    // Holding nothing, the run ends with its source: nothing to stop, and its
    // status is read only when asked for.
    if (PERFETTO_UNLIKELY(flags & State::kSourceDone)) {
      return nullptr;
    }
    batch = s.source->Next(s.output, *s.source_state);
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
  for (const State::Link& link : *s.output_links) {
    if (PERFETTO_UNLIKELY(!link.op->Process(*batch, *link.state))) {
      TransformFailed(link, s);
      return nullptr;
    }
    // A batch with no rows left has nothing for later transforms to do.
    if (PERFETTO_UNLIKELY(!batch->size())) {
      // Nor is there a next batch to pull, if the source said this was its
      // last and nothing needs its run ended.
      if (batch->last() && !(flags & State::kSegmented) && !s.holds_any) {
        SourceDone(s);
        return nullptr;
      }
      return NextSlow(s);
    }
  }
  // Whatever holds state must see the pull which finds the end.
  if (PERFETTO_UNLIKELY(s.holds_any)) {
    batch->SetLast(false);
  }
  return batch;
}

PERFETTO_NO_INLINE RowBatch* Pipeline::NextSlow(State& s) const {
  auto last = static_cast<uint32_t>(segments_.size() - 1);
  RowBatch* batch = SegmentNext(last, s.output, s);
  if (!batch) {
    Stop(s);
  } else {
    // The slow path may be ending runs or holding rows, which only a pull
    // finding the end stops.
    batch->SetLast(false);
  }
  return batch;
}

PERFETTO_ALWAYS_INLINE RowBatch* Pipeline::SegmentNext(uint32_t segment,
                                                       RowBatch& scratch,
                                                       State& s) const {
  const std::vector<State::Link>& links =
      segment == 0 ? s.source_links : s.starts[segment].links;
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
      start.current = SegmentNext(segment - 1, start.input, s);
      if (start.current) {
        has_input = true;
      } else if (s.flags & (State::kStopped | State::kFailed)) {
        return nullptr;
      } else {
        start.input_done = true;
      }
    }
    start.out.Reset();
    OpResult result = has_input
                          ? op.Execute(*start.current, start.out, op_state)
                          : op.Finish(start.out, op_state);
    if (result == OpResult::kError) {
      StepFailed(op.status(op_state), s);
      return nullptr;
    }
    start.continuation = has_input && result == OpResult::kHaveMoreOutput;
    if (result == OpResult::kFinished ||
        (!has_input && result != OpResult::kHaveMoreOutput)) {
      start.finished = true;
    }
    if (start.out.size()) {
      return &start.out;
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

base::Status Pipeline::status(const OperatorState& state) const {
  const auto& s = state.Cast<const State>();
  if (!(s.flags & (State::kHolds | State::kFailed)) &&
      (s.flags & State::kSourceDone) && !s.source_holds) {
    return source_.status(*s.source_state);
  }
  return s.status;
}

PERFETTO_ALWAYS_INLINE bool Pipeline::CanContinue(State& state) const {
  return PERFETTO_LIKELY(!(state.flags & (State::kStopped | State::kFailed)));
}

PERFETTO_NO_INLINE void Pipeline::Stop(State& state) const {
  for (auto& start : state.starts) {
    start.current = nullptr;
  }
  state.flags |= State::kStopped;
}

PERFETTO_NO_INLINE void Pipeline::SourceDone(State& s) const {
  s.flags |= State::kSourceDone;
  // Read now if ending the run resets it, or the pipeline holds rows and must
  // stop at once; otherwise status() reads it only if asked.
  if (s.source_holds || (s.flags & State::kHolds)) {
    Fail(s, source_.status(*s.source_state));
  }
  EndUpTo(0, s);
}

PERFETTO_NO_INLINE void Pipeline::EndRunsUpTo(uint32_t segment, State& s) {
  for (uint32_t seg = s.ended_segments; seg <= segment; ++seg) {
    OperatorState::EndRuns(s.held[seg]);
  }
  s.ended_segments = segment + 1;
}

void Pipeline::EndStateRun(void* state) {
  auto& s = *static_cast<State*>(state);
  auto last = static_cast<uint32_t>(s.held.size() - 1);
  if (s.ended_segments <= last) {
    EndRunsUpTo(last, s);
  }
}

void Pipeline::Fail(State& state, base::Status status) {
  if (!status.ok()) {
    state.status = std::move(status);
    state.flags |= State::kFailed;
  }
}

void Pipeline::StepFailed(base::Status status, State& state) {
  Fail(state, status.ok() ? base::ErrStatus("operator failed without status")
                          : std::move(status));
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
