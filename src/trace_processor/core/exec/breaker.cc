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
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

Breaker::~Breaker() = default;
Breaker::State::~State() = default;

std::unique_ptr<OperatorState> Breaker::MakeState() const {
  return CreateState();
}

OpResult Breaker::Execute(const RowBatch& in,
                          RowBatch& out,
                          OperatorState& state) const {
  out.Reset();
  State& s = state.Cast<State>();
  return s.status.ok() && Consume(in, s) ? OpResult::kNeedMoreInput
                                         : OpResult::kError;
}

OpResult Breaker::Finish(RowBatch& out, OperatorState& state) const {
  out.Reset();
  State& s = state.Cast<State>();
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  if (!s.filled) {
    if (!Finalize(s)) {
      return OpResult::kError;
    }
    s.filled = true;
  }
  if (Serve(out, s)) {
    return OpResult::kHaveMoreOutput;
  }
  return s.status.ok() ? OpResult::kNeedMoreInput : OpResult::kError;
}

void Breaker::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  Reset(s);
  s.status = base::OkStatus();
  s.filled = false;
}

base::Status Breaker::status(const OperatorState& state) const {
  const State& s = state.Cast<const State>();
  return s.status;
}

}  // namespace perfetto::trace_processor::core::exec
