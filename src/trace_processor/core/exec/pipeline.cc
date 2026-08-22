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

Pipeline::Pipeline(const Source& source,
                   std::vector<std::unique_ptr<Operator>> operators)
    : source_(source), operators_(std::move(operators)) {}

Pipeline::~Pipeline() = default;
Pipeline::State::~State() = default;

std::unique_ptr<OperatorState> Pipeline::MakeState() const {
  auto state = std::make_unique<State>();
  state->source = source_.MakeState();
  state->operators.reserve(operators_.size());
  for (const std::unique_ptr<Operator>& op : operators_) {
    state->operators.push_back(op->MakeState());
  }
  state->batches.resize(operators_.size());
  return state;
}

void Pipeline::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  s.pending.clear();
  source_.Rewind(*s.source);
  for (uint32_t i = 0; i < operators_.size(); ++i) {
    operators_[i]->Rewind(*s.operators[i]);
  }
}

base::Status Pipeline::status(const OperatorState& state) const {
  const State& s = state.Cast<const State>();
  for (uint32_t i = 0; i < operators_.size(); ++i) {
    base::Status status = operators_[i]->status(*s.operators[i]);
    if (!status.ok()) {
      return status;
    }
  }
  return source_.status(*s.source);
}

bool Pipeline::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (operators_.empty()) {
    return source_.GetData(out, *s.source);
  }
  for (;;) {
    uint32_t first;
    if (s.pending.empty()) {
      if (!source_.GetData(s.batches[0], *s.source)) {
        return false;
      }
      first = 0;
    } else {
      first = s.pending.back();
      s.pending.pop_back();
    }
    bool filled = true;
    for (uint32_t i = first; i < operators_.size(); ++i) {
      RowBatch& dst = i + 1 == operators_.size() ? out : s.batches[i + 1];
      OpResult result =
          operators_[i]->Execute(s.batches[i], dst, *s.operators[i]);
      if (result == OpResult::kError) {
        return false;
      }
      if (result == OpResult::kHaveMoreOutput) {
        s.pending.push_back(i);
      }
      if (dst.size() == 0) {
        filled = false;
        break;
      }
    }
    if (filled) {
      return true;
    }
  }
}

}  // namespace perfetto::trace_processor::core::exec
