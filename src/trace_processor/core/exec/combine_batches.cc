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

#include "src/trace_processor/core/exec/combine_batches.h"

#include <memory>

#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

CombineBatches::CombineBatches() = default;
CombineBatches::~CombineBatches() = default;
CombineBatches::State::~State() = default;

void CombineBatches::State::Reset() {
  buffered.Clear();
  flushed = false;
}

std::unique_ptr<OperatorState> CombineBatches::MakeState(
    Context& context) const {
  return std::make_unique<State>(context);
}

OpResult CombineBatches::Execute(const RowBatch& in,
                                 RowBatch& out,
                                 OperatorState& state) const {
  State& s = state.Cast<State>();
  bool large = in.size() > kSmallBatchRows;
  if (large || s.buffered.size() + in.size() > kTargetBatchRows) {
    // Output the buffered rows first to keep the row order.
    if (s.buffered.size() && !s.flushed) {
      s.buffered.Take(out);
      s.flushed = true;
      return OpResult::kHaveMoreOutput;
    }
    s.flushed = false;
    if (large) {
      out.CopyFrom(in);
      return OpResult::kNeedMoreInput;
    }
  }
  s.buffered.Append(in, *s.context);
  if (s.buffered.size() >= kTargetBatchRows) {
    s.buffered.Take(out);
  } else {
    out.Reset();
  }
  return OpResult::kNeedMoreInput;
}

OpResult CombineBatches::Finish(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (s.buffered.size()) {
    s.buffered.Take(out);
  } else {
    out.Reset();
  }
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
