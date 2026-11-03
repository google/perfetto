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

#include "src/trace_processor/core/exec/limit.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

Limit::Limit(std::optional<uint32_t> limit, uint32_t offset)
    : limit_(limit), offset_(offset) {}

Limit::~Limit() = default;
Limit::State::~State() = default;

void Limit::State::Reset() {
  skipped = 0;
  kept = 0;
}

std::unique_ptr<OperatorState> Limit::MakeState(Context&) const {
  return std::make_unique<State>();
}

OpResult Limit::Execute(const RowBatch& in,
                        RowBatch& out,
                        OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t size = in.size();
  uint32_t skip = std::min(offset_ - s.skipped, size);
  s.skipped += skip;
  uint32_t limit = limit_.value_or(std::numeric_limits<uint32_t>::max());
  uint32_t keep = std::min(size - skip, limit - s.kept);
  s.kept += keep;
  if (keep == 0) {
    out.Reset();
  } else {
    out.CopyFrom(in);
    if (keep != size) {
      out.mutable_selection().KeepRange(skip, keep);
    }
  }
  return limit_ && s.kept == *limit_ ? OpResult::kFinished
                                     : OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
