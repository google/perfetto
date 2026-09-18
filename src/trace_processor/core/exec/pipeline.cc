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
#include <utility>

namespace perfetto::trace_processor::core::exec {

Pipeline::Pipeline(const Source& source,
                   std::vector<std::unique_ptr<Operator>> operators,
                   ExecutionOptions options)
    : options_(std::move(options)),
      source_(source),
      operators_(std::move(operators)) {
  PERFETTO_CHECK(options_.target_batch_rows > 0 &&
                 options_.target_batch_rows <= kMaxBatchRows);
  PERFETTO_CHECK(options_.small_batch_rows <= options_.target_batch_rows);
}
Pipeline::~Pipeline() = default;
Pipeline::State::~State() = default;

std::unique_ptr<OperatorState> Pipeline::MakeState() const {
  auto state = std::make_unique<State>();
  state->source = source_.MakeState();
  for (const auto& op : operators_)
    state->operators.push_back(op->MakeState());
  state->stages.resize(operators_.size() + 1);
  return state;
}

void Pipeline::Stop(State& state) const {
  for (auto& stage : state.stages) {
    stage.input.Reset();
    stage.deferred.Reset();
    stage.scratch.Reset();
    stage.buffered.Clear();
  }
  state.stopped = true;
}
bool Pipeline::Check(State& state) const {
  if (!state.status.ok() || state.stopped)
    return false;
  if (options_.cancelled && options_.cancelled()) {
    state.status = base::ErrStatus("pipeline cancelled");
    Stop(state);
    return false;
  }
  return true;
}
void Pipeline::Rewind(OperatorState& state) const {
  auto& s = state.Cast<State>();
  Stop(s);
  s.stopped = false;
  s.emitted = 0;
  s.source_done = false;
  s.status = base::OkStatus();
  for (auto& stage : s.stages) {
    stage.done = false;
    stage.continuation = false;
  }
  source_.Rewind(*s.source);
  for (uint32_t i = 0; i < operators_.size(); ++i)
    operators_[i]->Rewind(*s.operators[i]);
}
base::Status Pipeline::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

// Pull an unbatched output from the prefix with `stage` operators. Each stage
// retains its input while a continuation is pending. Finish is only reached on
// successful exhaustion of that prefix, never on failure/cancellation.
bool Pipeline::Pull(uint32_t stage, RowBatch& out, State& s) const {
  if (!Check(s))
    return false;
  out.Reset();
  if (stage == 0) {
    if (s.source_done)
      return false;
    if (source_.GetData(out, *s.source))
      return true;
    s.source_done = true;
    s.status = source_.status(*s.source);
    return false;
  }
  auto& current = s.stages[stage - 1];
  if (current.done)
    return false;
  auto& op = *operators_[stage - 1];
  for (;;) {
    if (!Check(s))
      return false;
    bool has_input = current.continuation;
    if (!has_input) {
      current.input.Reset();
      has_input = Input(stage - 1, current.input, s, op.batch_preference());
    }
    if (!s.status.ok() || s.stopped)
      return false;
    OpResult result =
        has_input ? op.Execute(current.input, out, *s.operators[stage - 1])
                  : op.Finish(out, *s.operators[stage - 1]);
    if (result == OpResult::kError) {
      s.status = op.status(*s.operators[stage - 1]);
      if (s.status.ok())
        s.status = base::ErrStatus("operator failed without status");
      out.Reset();
      return false;
    }
    current.continuation = has_input && result == OpResult::kHaveMoreOutput;
    if (!has_input && result != OpResult::kHaveMoreOutput)
      current.done = true;
    if (out.size())
      return true;
    if (current.done)
      return false;
  }
}

bool Pipeline::Input(uint32_t boundary,
                     RowBatch& out,
                     State& s,
                     BatchPreference preference) const {
  auto& stage = s.stages[boundary];
  bool combine = preference == BatchPreference::kThroughput &&
                 options_.limit == std::numeric_limits<uint64_t>::max();
  for (;;) {
    if (!Check(s)) {
      out.Reset();
      return false;
    }
    auto& next = stage.scratch;
    next.Reset();
    if (stage.deferred.size()) {
      next.CopyFrom(stage.deferred);
      stage.deferred.Reset();
    } else if (!Pull(boundary, next, s)) {
      if (!s.status.ok() || s.stopped) {
        stage.buffered.Clear();
        out.Reset();
        return false;
      }
      if (stage.buffered.size()) {
        stage.buffered.Take(out);
        return true;
      }
      return false;
    }
    if (!next.size())
      continue;
    if (!combine) {
      out.SwapContents(next);
      return true;
    }
    if (next.size() > options_.small_batch_rows ||
        stage.buffered.size() + next.size() > options_.target_batch_rows) {
      if (stage.buffered.size()) {
        stage.deferred.CopyFrom(next);
        stage.buffered.Take(out);
        return true;
      }
      out.SwapContents(next);
      return true;
    }
    s.status = stage.buffered.Append(next);
    if (!s.status.ok()) {
      stage.buffered.Clear();
      out.Reset();
      return false;
    }
    if (stage.buffered.size() >= options_.target_batch_rows) {
      stage.buffered.Take(out);
      return true;
    }
  }
}

bool Pipeline::GetData(RowBatch& out, OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (s.emitted >= options_.limit) {
    Stop(s);
    return false;
  }
  if (!Input(static_cast<uint32_t>(operators_.size()), out, s,
             options_.preference)) {
    Stop(s);
    return false;
  }
  uint64_t remaining = options_.limit - s.emitted;
  if (out.size() > remaining)
    out.Slice(RowSelection::Range(), static_cast<uint32_t>(remaining));
  s.emitted += out.size();
  if (s.emitted == options_.limit)
    Stop(s);
  return true;
}
}  // namespace perfetto::trace_processor::core::exec
