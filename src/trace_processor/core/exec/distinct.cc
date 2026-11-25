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

#include "src/trace_processor/core/exec/distinct.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

Distinct::Distinct(core::ArenaSpan<uint32_t> columns) : columns_(columns) {
  PERFETTO_CHECK(!columns_.empty() && columns_.size() <= kMaxColumns);
}

OperatorState* Distinct::NewState(Context& context) const {
  auto* state = context.Make<State>();
  state->row_keys = context.NewSpan<uint32_t>(kMaxBatchRows);
  state->kept = context.NewSpan<uint32_t>(kMaxBatchRows);
  return state;
}

bool Distinct::Process(RowBatch& batch, OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t count = batch.size();
  if (count == 0) {
    return true;
  }
  uint32_t next = s.keys.size();
  if (std::optional<uint32_t> bad =
          s.keys.FindOrCreate(batch, columns_.span(), s.row_keys.begin(), s)) {
    s.context().Fail(base::ErrStatus(
        "DISTINCT: key %u must be a column holding one type", *bad + 1));
    return false;
  }
  if (s.keys.size() == next + count) {
    return true;
  }
  // Keys are numbered in the order first seen, so a row is the first with
  // its key exactly when it got the next number.
  // Use locals, since a store to `kept` could otherwise alias the spans' sizes.
  const uint32_t* row_keys = s.row_keys.begin();
  uint32_t* kept = s.kept.begin();
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    kept[n] = i;
    bool first = row_keys[i] == next;
    n += first;
    next += first;
  }
  batch.mutable_selection().Keep(base::Span<const uint32_t>(kept, kept + n));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
