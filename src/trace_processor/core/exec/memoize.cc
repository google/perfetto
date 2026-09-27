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

#include "src/trace_processor/core/exec/memoize.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"

namespace perfetto::trace_processor::core::exec {
namespace {

struct MemoizeState : OperatorState {
  ~MemoizeState() override;

  std::unique_ptr<OperatorState> source;
  RowBatch batch;
  RowStore kept;
  // Whether `kept` holds all the source's rows.
  bool complete = false;
  // Whether this read is replaying `kept`, and how far it has got.
  bool replaying = false;
  uint32_t served = 0;
  // Whether this read is keeping what it reads.
  bool keeping = true;
};

MemoizeState::~MemoizeState() = default;

}  // namespace

Memoize::Memoize(const Source& source) : source_(source) {}
Memoize::~Memoize() = default;

std::unique_ptr<OperatorState> Memoize::MakeState() const {
  auto state = std::make_unique<MemoizeState>();
  state->source = source_.MakeState();
  return state;
}

bool Memoize::GetData(RowBatch& out, OperatorState& state) const {
  auto& s = state.Cast<MemoizeState>();
  if (s.replaying) {
    uint32_t left = s.kept.size() - s.served;
    if (left == 0) {
      return false;
    }
    s.served += s.kept.View(&out, s.served, std::min(left, kMaxBatchRows));
    return true;
  }
  if (!source_.GetData(s.batch, *s.source)) {
    s.complete = s.keeping && source_.status(*s.source).ok();
    return false;
  }
  // Batches which cannot be kept are still passed on: this read just keeps
  // nothing.
  if (s.keeping && !s.kept.Append(s.batch).ok()) {
    s.keeping = false;
    s.kept.Clear();
  }
  out.CopyFrom(s.batch);
  return true;
}

void Memoize::Rewind(OperatorState& state) const {
  auto& s = state.Cast<MemoizeState>();
  if (s.complete) {
    s.replaying = true;
    s.served = 0;
    return;
  }
  source_.Rewind(*s.source);
  s.kept.Clear();
  s.keeping = true;
}

base::Status Memoize::status(const OperatorState& state) const {
  const auto& s = state.Cast<const MemoizeState>();
  return s.replaying ? base::OkStatus() : source_.status(*s.source);
}

}  // namespace perfetto::trace_processor::core::exec
