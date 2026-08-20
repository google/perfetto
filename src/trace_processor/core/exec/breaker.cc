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

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

BreakerState::~BreakerState() = default;

Breaker::~Breaker() = default;

std::unique_ptr<OperatorState> Breaker::MakeState() const {
  std::unique_ptr<BreakerState> state = MakeBreakerState();
  state->input = input_.MakeState();
  return state;
}

void Breaker::Rewind(OperatorState& state) const {
  BreakerState& s = state.Cast<BreakerState>();
  input_.Rewind(*s.input);
  Rewind(s);
  s.filled = false;
  s.status = base::OkStatus();
}

base::Status Breaker::status(const OperatorState& state) const {
  const BreakerState& s = state.Cast<const BreakerState>();
  return s.status.ok() ? input_.status(*s.input) : s.status;
}

base::Status Breaker::Fill(BreakerState& state) const {
  while (input_.GetData(state.input_batch, *state.input)) {
    RETURN_IF_ERROR(Consume(state.input_batch, state));
  }
  RETURN_IF_ERROR(input_.status(*state.input));
  return Finish(state);
}

bool Breaker::GetData(RowBatch& out, OperatorState& state) const {
  BreakerState& s = state.Cast<BreakerState>();
  if (!s.filled) {
    s.filled = true;
    s.status = Fill(s);
    if (!s.status.ok()) {
      return false;
    }
  }
  return Emit(out, s);
}

}  // namespace perfetto::trace_processor::core::exec
