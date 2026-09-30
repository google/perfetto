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
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

Pipeline::Pipeline(const Source& source,
                   std::vector<std::unique_ptr<Operator>> operators,
                   ExecutionOptions options)
    : source_(source),
      operators_(std::move(operators)),
      options_(std::move(options)) {
  PERFETTO_CHECK(options_.target_batch_rows > 0 &&
                 options_.target_batch_rows <= kMaxBatchRows);
  PERFETTO_CHECK(options_.small_batch_rows <= options_.target_batch_rows);
  has_limit_ = options_.limit != std::numeric_limits<uint64_t>::max();
  has_cancel_ = static_cast<bool>(options_.cancelled);
  combine_output_ =
      options_.preference == BatchPreference::kThroughput && !has_limit_;
  segments_.emplace_back();
  for (uint32_t i = 0; i < operators_.size(); ++i) {
    if (operators_[i]->Rewinds()) {
      rewound_.push_back(i);
    }
    if (const Transform* transform = operators_[i]->AsTransform()) {
      segments_.back().transforms.push_back(i);
      segments_.back().transform_ops.push_back(transform);
    } else {
      segments_.push_back({static_cast<int32_t>(i), {}, {}});
    }
  }
  fused_ = segments_.size() == 1 && !has_limit_ && !has_cancel_ &&
           !combine_output_;
}
Pipeline::~Pipeline() = default;
Pipeline::State::~State() = default;

std::unique_ptr<OperatorState> Pipeline::MakeState() const {
  auto state = std::make_unique<State>();
  state->source = source_.MakeState();
  for (const auto& op : operators_)
    state->operators.push_back(op->MakeState());
  state->starts.resize(segments_.size());
  if (fused_) {
    for (uint32_t i = 0; i < operators_.size(); ++i) {
      const Transform* op = segments_[0].transform_ops[i];
      state->chain.push_back(
          {op->process_fn(), op, state->operators[i].get()});
    }
  }
  return state;
}

void Pipeline::Stop(State& state) const {
  for (auto& start : state.starts) {
    start.input.deferred = nullptr;
    start.input.buffered.Clear();
    start.current = nullptr;
  }
  state.output.deferred = nullptr;
  state.output.buffered.Clear();
  state.stopped = true;
}

PERFETTO_ALWAYS_INLINE bool Pipeline::CanContinue(State& state) const {
  if (PERFETTO_UNLIKELY(!state.status.ok() || state.stopped))
    return false;
  if (has_cancel_ && options_.cancelled()) {
    state.status = base::ErrStatus("pipeline cancelled");
    Stop(state);
    return false;
  }
  return true;
}

void Pipeline::Rewind(OperatorState& state) const {
  auto& s = state.Cast<State>();
  ++s.run;
  if (combine_output_) {
    s.output.deferred = nullptr;
    if (s.output.buffered.size()) {
      s.output.buffered.Clear();
    }
  }
  s.stopped = false;
  s.emitted = 0;
  s.source_done = false;
  s.source_open = false;
  if (!s.status.ok()) {
    s.status = base::OkStatus();
  }
  for (uint32_t i : rewound_)
    operators_[i]->Rewind(*s.operators[i]);
}

// Out of line, so the statuses it builds stay off the hot path's stack.
PERFETTO_NO_INLINE void Pipeline::ChainFailed(const Transform& op,
                                              OperatorState& op_state,
                                              State& s) const {
  s.status = op.status(op_state);
  if (s.status.ok())
    s.status = base::ErrStatus("operator failed without status");
  s.source_done = true;
}

PERFETTO_NO_INLINE void Pipeline::ResetStatus(State& s) {
  s.status = base::OkStatus();
}

PERFETTO_ALWAYS_INLINE RowBatch* Pipeline::Chain(RowBatch* batch,
                                                 RowBatch& scratch,
                                                 State& s) const {
  for (;;) {
    if (!batch) {
      // Running out is the common case: the source's status is read only
      // when asked for, by status().
      s.source_done = true;
      return nullptr;
    }
    for (const State::Link& link : s.chain) {
      if (PERFETTO_UNLIKELY(!link.process(*link.op, *batch, *link.state))) {
        ChainFailed(*link.op, *link.state, s);
        return nullptr;
      }
    }
    if (batch->size()) {
      return batch;
    }
    if (batch->last()) {
      s.source_done = true;
      return nullptr;
    }
    batch = source_.Next(scratch, *s.source);
  }
}

RowBatch* Pipeline::Open(RowBatch& scratch, OperatorState& state) const {
  if (fused_) {
    auto& s = state.Cast<State>();
    for (uint32_t i : rewound_)
      operators_[i]->Rewind(*s.operators[i]);
    if (PERFETTO_UNLIKELY(!s.status.ok())) {
      ResetStatus(s);
    }
    s.source_done = false;
    s.source_open = true;
    RowBatch& own = s.output.scratch;
    return Chain(source_.Open(own, *s.source), own, s);
  }
  Pipeline::Rewind(state);
  return Pipeline::Next(scratch, state);
}

PERFETTO_ALWAYS_INLINE RowBatch* Pipeline::SegmentNext(uint32_t segment,
                                                       RowBatch& scratch,
                                                       State& s) const {
  const Segment& seg = segments_[segment];
  for (;;) {
    if (!CanContinue(s)) {
      return nullptr;
    }
    RowBatch* batch;
    if (seg.start < 0) {
      if (s.source_done) {
        return nullptr;
      }
      if (s.source_open) {
        batch = source_.Next(scratch, *s.source);
      } else {
        s.source_open = true;
        batch = source_.Open(scratch, *s.source);
      }
      if (!batch) {
        s.source_done = true;
        s.status = source_.status(*s.source);
        return nullptr;
      }
    } else {
      batch = StartNext(segment, s);
      if (!batch) {
        return nullptr;
      }
    }
    for (size_t t = 0; t < seg.transforms.size(); ++t) {
      uint32_t i = seg.transforms[t];
      const Transform& transform = *seg.transform_ops[t];
      if (!transform.Process(*batch, *s.operators[i])) {
        s.status = transform.status(*s.operators[i]);
        if (s.status.ok())
          s.status = base::ErrStatus("operator failed without status");
        return nullptr;
      }
    }
    if (batch->size()) {
      return batch;
    }
    if (batch->last()) {
      return nullptr;
    }
  }
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
  const Operator& op = *operators_[index];
  OperatorState& op_state = *s.operators[index];
  for (;;) {
    if (!CanContinue(s) || start.finished) {
      return nullptr;
    }
    bool has_input = start.continuation;
    if (!has_input && !start.input_done) {
      start.current = Read(segment - 1, op.batch_preference(), start.input, s);
      if (start.current) {
        has_input = true;
      } else if (!s.status.ok() || s.stopped) {
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
      s.status = op.status(op_state);
      if (s.status.ok())
        s.status = base::ErrStatus("operator failed without status");
      return nullptr;
    }
    start.continuation = has_input && result == OpResult::kHaveMoreOutput;
    if (!has_input && result != OpResult::kHaveMoreOutput)
      start.finished = true;
    if (start.out.size()) {
      start.out.set_last(start.finished);
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
        if (!s.status.ok() || s.stopped) {
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
    s.status = c.buffered.Append(*next);
    if (!s.status.ok()) {
      c.buffered.Clear();
      return nullptr;
    }
    if (c.buffered.size() >= options_.target_batch_rows) {
      c.buffered.Take(c.combined);
      return &c.combined;
    }
  }
}

RowBatch* Pipeline::Next(RowBatch&, OperatorState& state) const {
  auto& s = state.Cast<State>();
  if (fused_) {
    if (s.source_done) {
      return nullptr;
    }
    RowBatch& own = s.output.scratch;
    // Opened by the first pull of a run when not through Open().
    if (!s.source_open) {
      s.source_open = true;
      return Chain(source_.Open(own, *s.source), own, s);
    }
    return Chain(source_.Next(own, *s.source), own, s);
  }
  if (has_limit_ && s.emitted >= options_.limit) {
    Stop(s);
    return nullptr;
  }
  auto last = static_cast<uint32_t>(segments_.size() - 1);
  RowBatch* batch = combine_output_
                        ? Read(last, options_.preference, s.output, s)
                        : SegmentNext(last, s.output.scratch, s);
  if (!batch) {
    Stop(s);
    return nullptr;
  }
  if (!has_limit_) {
    return batch;
  }
  uint64_t remaining = options_.limit - s.emitted;
  if (batch->size() > remaining)
    batch->Slice(RowSelection::Range(), static_cast<uint32_t>(remaining));
  s.emitted += batch->size();
  if (s.emitted == options_.limit) {
    batch->set_last(true);
    Stop(s);
  }
  return batch;
}

bool Pipeline::GetData(RowBatch& out, OperatorState& state) const {
  // Let go of the caller's last batch first, so the storage it held can be
  // reused for this one.
  out.Reset();
  RowBatch* batch = Next(out, state);
  if (!batch) {
    out.Reset();
    return false;
  }
  // Swapped rather than copied, so storage the caller let go of returns to
  // the batch it came from.
  if (batch != &out) {
    out.SwapContents(*batch);
  }
  return true;
}

base::Status Pipeline::status(const OperatorState& state) const {
  const auto& s = state.Cast<const State>();
  if (!s.status.ok() || !fused_) {
    return s.status;
  }
  return source_.status(*s.source);
}

}  // namespace perfetto::trace_processor::core::exec
