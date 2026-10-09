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

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr size_t kTransformStep =
    base::variant_index<Pipeline::Step, std::unique_ptr<Transform>>();
constexpr size_t kOperatorStep =
    base::variant_index<Pipeline::Step, std::unique_ptr<Operator>>();

}  // namespace

Pipeline::Pipeline(const Source& source,
                   std::vector<Step> steps,
                   ExecutionOptions options)
    : source_(source), steps_(std::move(steps)), options_(std::move(options)) {
  PERFETTO_CHECK(options_.target_batch_rows > 0 &&
                 options_.target_batch_rows <= kMaxBatchRows);
  PERFETTO_CHECK(options_.small_batch_rows <= options_.target_batch_rows);
  if (options_.limit != std::numeric_limits<uint64_t>::max()) {
    plan_flags_ |= State::kLimit;
  }
  if (options_.cancelled) {
    plan_flags_ |= State::kCancellable;
  }
  if (options_.preference == BatchPreference::kThroughput &&
      !(plan_flags_ & State::kLimit)) {
    plan_flags_ |= State::kCombineOutput;
  }
  segments_.emplace_back();
  for (uint32_t i = 0; i < steps_.size(); ++i) {
    switch (steps_[i].index()) {
      case kTransformStep:
        segments_.back().transforms.push_back(i);
        segments_.back().transform_ops.push_back(
            base::unchecked_get<std::unique_ptr<Transform>>(steps_[i]).get());
        break;
      case kOperatorStep: {
        const Operator* op =
            base::unchecked_get<std::unique_ptr<Operator>>(steps_[i]).get();
        segments_.push_back(
            {static_cast<int32_t>(i), op, op->traits().preference, {}, {}});
        break;
      }
    }
  }
  last_segment_ = static_cast<uint32_t>(segments_.size() - 1);
  if (segments_.size() > 1) {
    plan_flags_ |= State::kSegmented;
  }
}
Pipeline::~Pipeline() = default;
Pipeline::State::~State() = default;

std::unique_ptr<OperatorState> Pipeline::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->context = &context;
  state->owned_source_state = source_.MakeState(context);
  state->source = &source_;
  state->source_state = state->owned_source_state.get();
  for (const Step& step : steps_) {
    switch (step.index()) {
      case kTransformStep:
        state->operators.push_back(
            base::unchecked_get<std::unique_ptr<Transform>>(step)->MakeState(
                context));
        break;
      case kOperatorStep:
        state->operators.push_back(
            base::unchecked_get<std::unique_ptr<Operator>>(step)->MakeState(
                context));
        break;
    }
  }
  state->starts.resize(segments_.size());
  for (size_t seg = 0; seg < segments_.size(); ++seg) {
    const Segment& segment = segments_[seg];
    auto& links = seg == 0 ? state->source_links : state->starts[seg].links;
    for (size_t t = 0; t < segment.transforms.size(); ++t) {
      links.push_back({segment.transform_ops[t],
                       state->operators[segment.transforms[t]].get()});
    }
  }
  for (auto& op_state : state->operators) {
    if (op_state->reset_each_run()) {
      state->reset_each_run.push_back(op_state.get());
    }
  }
  state->output_links = last_segment_ == 0
                            ? &state->source_links
                            : &state->starts[last_segment_].links;
  state->flags = plan_flags_;
  return state;
}

void Pipeline::Rewind(OperatorState& state) const {
  auto& s = state.Cast<State>();
  ++s.run;
  if (s.flags & State::kCombineOutput) {
    s.output.deferred = nullptr;
    if (s.output.buffered.size()) {
      s.output.buffered.Clear();
    }
  }
  if (s.flags & State::kFailed) {
    s.status = base::OkStatus();
  }
  s.flags = plan_flags_;
  s.emitted = 0;
  source_.Rewind(*s.source_state);
  for (OperatorState* op_state : s.reset_each_run) {
    op_state->Reset();
  }
}

// A batch is one pull, from the source or the operator starting the last
// segment, and that segment's transforms. Limits, combining, cancellation and
// failure are NextSlow()'s.
RowBatch* Pipeline::Next(RowBatch&, OperatorState& state) const {
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
    batch = s.source->Next(s.output.scratch, *s.source_state);
    if (PERFETTO_UNLIKELY(!batch)) {
      s.flags |= State::kSourceDone;
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
  }
  return PERFETTO_LIKELY(batch->size()) ? batch : NextSlow(s);
}

PERFETTO_NO_INLINE RowBatch* Pipeline::NextSlow(State& s) const {
  if ((s.flags & State::kLimit) && s.emitted >= options_.limit) {
    Stop(s);
    return nullptr;
  }
  auto last = static_cast<uint32_t>(segments_.size() - 1);
  RowBatch* batch = (s.flags & State::kCombineOutput)
                        ? Read(last, options_.preference, s.output, s)
                        : SegmentNext(last, s.output.scratch, s);
  if (!batch) {
    Stop(s);
    return nullptr;
  }
  if (!(s.flags & State::kLimit)) {
    return batch;
  }
  uint64_t remaining = options_.limit - s.emitted;
  if (batch->size() > remaining)
    batch->mutable_selection().KeepRange(0, static_cast<uint32_t>(remaining));
  s.emitted += batch->size();
  if (s.emitted == options_.limit)
    Stop(s);
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
        s.flags |= State::kSourceDone;
        if (s.flags & State::kHolds) {
          SourceStatus(s);
        }
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
    s.flags |= State::kSourceDone;
    if (s.flags & State::kHolds) {
      SourceStatus(s);
    }
  }
  return batch;
}

RowBatch* Pipeline::StartNext(uint32_t segment, State& s) const {
  auto& start = s.starts[segment];
  if (start.run != s.run) {
    start.input.deferred = nullptr;
    if (start.input.buffered.size()) {
      start.input.buffered.Clear();
    }
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
      return nullptr;
    }
    bool has_input = start.continuation;
    if (!has_input && !start.input_done) {
      start.current =
          Read(segment - 1, segments_[segment].preference, start.input, s);
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
    if (!has_input && result != OpResult::kHaveMoreOutput)
      start.finished = true;
    if (start.out.size()) {
      return &start.out;
    }
    if (start.finished) {
      return nullptr;
    }
  }
}

PERFETTO_ALWAYS_INLINE RowBatch* Pipeline::Read(uint32_t segment,
                                                BatchPreference preference,
                                                Combiner& c,
                                                State& s) const {
  bool combine = preference == BatchPreference::kThroughput &&
                 options_.limit == std::numeric_limits<uint64_t>::max();
  if (!combine) {
    return SegmentNext(segment, c.scratch, s);
  }
  for (;;) {
    RowBatch* next;
    if (c.deferred) {
      next = c.deferred;
      c.deferred = nullptr;
    } else {
      next = SegmentNext(segment, c.scratch, s);
      if (!next) {
        if (s.flags & (State::kStopped | State::kFailed)) {
          c.buffered.Clear();
          return nullptr;
        }
        if (c.buffered.size()) {
          c.buffered.Take(c.combined);
          return &c.combined;
        }
        return nullptr;
      }
    }
    if (next->size() > options_.small_batch_rows ||
        c.buffered.size() + next->size() > options_.target_batch_rows) {
      if (c.buffered.size()) {
        c.deferred = next;
        c.buffered.Take(c.combined);
        return &c.combined;
      }
      return next;
    }
    base::Status appended = c.buffered.Append(*next, *s.context);
    if (!appended.ok()) {
      Fail(s, std::move(appended));
      c.buffered.Clear();
      return nullptr;
    }
    if (c.buffered.size() >= options_.target_batch_rows) {
      c.buffered.Take(c.combined);
      return &c.combined;
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
      (s.flags & State::kSourceDone)) {
    return source_.status(*s.source_state);
  }
  return s.status;
}

PERFETTO_ALWAYS_INLINE bool Pipeline::CanContinue(State& state) const {
  if (PERFETTO_UNLIKELY(state.flags & (State::kStopped | State::kFailed)))
    return false;
  if ((state.flags & State::kCancellable) && options_.cancelled()) {
    Fail(state, base::ErrStatus("pipeline cancelled"));
    Stop(state);
    return false;
  }
  return true;
}

PERFETTO_NO_INLINE void Pipeline::Stop(State& state) const {
  for (auto& start : state.starts) {
    start.input.deferred = nullptr;
    if (start.input.buffered.size()) {
      start.input.buffered.Clear();
    }
    start.current = nullptr;
  }
  state.output.deferred = nullptr;
  if (state.output.buffered.size()) {
    state.output.buffered.Clear();
  }
  state.flags |= State::kStopped;
}

PERFETTO_NO_INLINE void Pipeline::SourceStatus(State& s) const {
  Fail(s, source_.status(*s.source_state));
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

}  // namespace perfetto::trace_processor::core::exec
