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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

Distinct::Distinct(std::vector<uint32_t> columns)
    : columns_(std::move(columns)) {
  PERFETTO_CHECK(!columns_.empty() && columns_.size() <= kMaxColumns);
}

Distinct::~Distinct() = default;

Distinct::State::~State() = default;

void Distinct::State::Reset() {
  keys.Clear();
  status = base::OkStatus();
}

std::unique_ptr<OperatorState> Distinct::MakeState(Context&) const {
  return std::make_unique<State>();
}

base::Status Distinct::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

bool Distinct::Process(RowBatch& batch, OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t count = batch.size();
  if (count == 0) {
    return true;
  }
  s.row_keys.resize(count);
  uint32_t next = s.keys.size();
  if (std::optional<uint32_t> bad =
          s.keys.FindOrCreate(batch, columns_, s.row_keys.data())) {
    s.status = base::ErrStatus(
        "DISTINCT: key %u must be a column holding one type", *bad + 1);
    return false;
  }
  if (s.keys.size() == next + count) {
    return true;
  }
  // Keys are numbered in the order first seen, so a row is the first with
  // its key exactly when it got the next number.
  s.kept.resize(count);
  uint32_t* kept = s.kept.data();
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    kept[n] = i;
    bool first = s.row_keys[i] == next;
    n += first;
    next += first;
  }
  batch.mutable_selection().Keep(base::Span<const uint32_t>(kept, kept + n));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
