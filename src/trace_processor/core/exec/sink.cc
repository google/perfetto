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

#include "src/trace_processor/core/exec/sink.h"

#include <cstdint>

#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"

namespace perfetto::trace_processor::core::exec {

// Defined out of line so that Sink's vtable is emitted in one object file.
bool Sink::FinalizeSink(OperatorState&) const {
  return true;
}

Pipelines::Pipelines() = default;
Pipelines::~Pipelines() = default;

uint32_t Pipelines::Add(const Pipeline* pipeline, const Sink* sink) {
  auto id = static_cast<uint32_t>(entries_.size());
  entries_.push_back({pipeline, sink});
  return id;
}

PipelineUse::PipelineUse(HeldState* owner, uint32_t id) : id_(id) {
  owner->AddHeld(this, &EndRun);
}

OperatorState* PipelineUse::Built(Context& context) {
  context_ = &context;
  return context.BuildSink(id_);
}

RowBatch* PipelineUse::Next(Context& context) {
  context_ = &context;
  return context.NextBatch(id_);
}

OperatorState* PipelineUse::IfBuilt() const {
  return context_ ? context_->IfBuilt(id_) : nullptr;
}

void PipelineUse::EndRun() {
  if (context_) {
    context_->EndPipelineRun(id_);
    context_ = nullptr;
  }
}

struct Served::State : HeldState {
  State(Context& c, uint32_t id) : context(&c), use(this, id) {}
  ~State() override;

  Context* context;
  PipelineUse use;
  RunValue<bool> done{this, false};
};

Served::State::~State() = default;

Served::Served(uint32_t id, const Breaker& breaker)
    : id_(id), breaker_(breaker) {}

OperatorState* Served::NewState(Context& context) const {
  return context.Make<State>(context, id_);
}

bool Served::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  out.Reset();
  if (s.done) {
    return false;
  }
  // Built the first time, and then only looked up.
  OperatorState* sink = s.use.Built(*s.context);
  if (!sink) {
    s.done = true;
    return false;
  }
  auto& built = sink->Cast<Breaker::State>();
  if (breaker_.Serve(out, built)) {
    return true;
  }
  // Served whole: what the breaker holds goes now, not when the run ends.
  s.done = true;
  s.use.EndRun();
  return false;
}

const RowsInPlace* Served::InPlace(const OperatorState& state) const {
  const State& s = state.Cast<const State>();
  OperatorState* built = s.use.IfBuilt();
  return built ? breaker_.InPlace(*built) : nullptr;
}

}  // namespace perfetto::trace_processor::core::exec
