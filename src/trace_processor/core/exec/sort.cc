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

#include "src/trace_processor/core/exec/sort.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/sorted_rows.h"

namespace perfetto::trace_processor::core::exec {

Sort::Sort(SortSpec spec) : spec_(std::move(spec)) {}
Sort::~Sort() = default;
Sort::State::~State() = default;

std::unique_ptr<Breaker::State> Sort::CreateState() const {
  return std::make_unique<State>();
}

bool Sort::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  base::Status status = s.rows.Append(in, spec_.keys);
  if (!status.ok()) {
    s.status = base::ErrStatus("SORT: %s", status.c_message());
    return false;
  }
  return true;
}

bool Sort::Finalize(Breaker::State& state) const {
  static_cast<State&>(state).rows.Sort();
  return true;
}

bool Sort::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (s.emitted == s.rows.size()) {
    return false;
  }
  s.emitted += s.rows.View(&out, s.emitted, *s.context);
  return true;
}

void Sort::State::Reset() {
  Breaker::State::Reset();
  rows.Clear();
  emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
