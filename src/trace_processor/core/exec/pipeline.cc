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
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

PullPipeline::PullPipeline(const Source& source,
                           std::vector<std::unique_ptr<Operator>> operators)
    : source_(source), operators_(std::move(operators)) {}

PullPipeline::~PullPipeline() = default;
PullPipeline::State::~State() = default;

std::unique_ptr<OperatorState> PullPipeline::MakeState() const {
  auto state = std::make_unique<State>();
  state->source = source_.MakeState();
  state->operators.reserve(operators_.size());
  for (const std::unique_ptr<Operator>& op : operators_) {
    state->operators.push_back(op->MakeState());
  }
  return state;
}

void PullPipeline::Rewind(OperatorState& state) const {
  source_.Rewind(*state.Cast<State>().source);
}

base::Status PullPipeline::status(const OperatorState& state) const {
  return source_.status(*state.Cast<const State>().source);
}

bool PullPipeline::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  while (source_.GetData(out, *s.source)) {
    bool dropped = false;
    for (uint32_t i = 0; i < operators_.size(); ++i) {
      if (operators_[i]->Execute(out, *s.operators[i]) == OpResult::kDrop) {
        dropped = true;
        break;
      }
    }
    if (!dropped) {
      return true;
    }
  }
  return false;
}

}  // namespace perfetto::trace_processor::core::exec
