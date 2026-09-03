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

#include "src/trace_processor/core/exec/breaker.h"

#include <memory>
#include <utility>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

Breaker::Breaker(const Source& input) : input_(input) {}
Breaker::~Breaker() = default;
Breaker::State::~State() = default;

std::unique_ptr<OperatorState> Breaker::MakeState() const {
  std::unique_ptr<State> state = CreateState();
  state->input = input_.MakeState();
  return state;
}

bool Breaker::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (!s.status.ok()) {
    return false;
  }
  if (!s.filled) {
    while (input_.GetData(s.batch, *s.input)) {
      if (!Consume(s.batch, s)) {
        return false;
      }
    }
    base::Status input_status = input_.status(*s.input);
    if (!input_status.ok()) {
      s.status = std::move(input_status);
      return false;
    }
    if (!Finish(s)) {
      return false;
    }
    s.filled = true;
  }
  return Serve(out, s);
}

void Breaker::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  input_.Rewind(*s.input);
  Reset(s);
  s.status = base::OkStatus();
  s.filled = false;
}

base::Status Breaker::status(const OperatorState& state) const {
  const State& s = state.Cast<const State>();
  return s.status.ok() ? input_.status(*s.input) : s.status;
}

}  // namespace perfetto::trace_processor::core::exec
