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

#include "src/trace_processor/core/exec/context.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

Context::Context() = default;

Context::~Context() {
  for (const Owned& owned : owned_) {
    owned.destroy(owned.object);
  }
  for (OperatorState* state : held_) {
    delete state->registry_;
  }
  nulls_ = ColumnBuffer();
}

const ColumnBuffer& Context::NullBuffer() {
  if (PERFETTO_UNLIKELY(!nulls_)) {
    // Shared by columns of every type, so as wide as the widest.
    nulls_ = TakeBuffer(ColumnChunk::kMaxValueSize);
    ColumnChunk& chunk = nulls_.chunk();
    memset(chunk.Values<uint8_t>(), 0,
           kMaxBatchRows * ColumnChunk::kMaxValueSize);
    chunk.validity = BitVector::CreateWithSize(kMaxBatchRows, false);
  }
  return nulls_;
}

void Context::SetPipelines(const Pipelines* pipelines) {
  PERFETTO_CHECK(!slots_);
  pipelines_ = pipelines;
  slot_count_ = pipelines ? pipelines->size() : 0;
  slots_ = std::make_unique<Slot[]>(slot_count_);
}

Context::Slot& Context::Start(uint32_t id) {
  PERFETTO_CHECK(id < slot_count_);
  Slot& slot = slots_[id];
  const Pipelines::Entry& entry = (*pipelines_)[id];
  if (!slot.pipeline) {
    slot.pipeline = entry.pipeline->MakeState(*this);
    if (entry.sink) {
      slot.state = entry.sink->MakeSinkState(*this);
    }
  } else {
    entry.pipeline->Rewind(*slot.pipeline);
  }
  slot.running = true;
  return slot;
}

OperatorState* Context::BuildSink(uint32_t id) {
  if (slots_[id].built) {
    return slots_[id].state;
  }
  Slot& slot = Start(id);
  const Pipelines::Entry& entry = (*pipelines_)[id];
  const Pipeline& pipeline = *entry.pipeline;
  const Sink& sink = *entry.sink;
  bool ok = true;
  while (RowBatch* batch = pipeline.Next(*slot.pipeline)) {
    if (!sink.ConsumeBatch(*batch, *slot.state)) {
      ok = false;
      break;
    }
  }
  if (ok && failed()) {
    ok = false;
  }
  if (ok && !sink.FinalizeSink(*slot.state)) {
    ok = false;
  }
  if (!ok) {
    if (!failed()) {
      Fail(base::ErrStatus("sink failed without saying why"));
    }
    EndPipelineRun(id);
    return nullptr;
  }
  slot.built = true;
  return slot.state;
}

OperatorState* Context::IfBuilt(uint32_t id) const {
  return slots_[id].built ? slots_[id].state : nullptr;
}

RowBatch* Context::NextBatch(uint32_t id) {
  Slot& slot = slots_[id].running ? slots_[id] : Start(id);
  const Pipelines::Entry& entry = (*pipelines_)[id];
  return entry.pipeline->Next(*slot.pipeline);
}

void Context::EndPipelineRun(uint32_t id) {
  Slot& slot = slots_[id];
  if (!slot.running) {
    return;
  }
  slot.running = false;
  slot.built = false;
  (*pipelines_)[id].pipeline->EndRun(*slot.pipeline);
  if (slot.state) {
    EndRun(*slot.state);
  }
}

void Context::Fail(base::Status status) {
  if (error_.ok()) {
    error_ = std::move(status);
  }
}

void Context::AddState(OperatorState* state) {
  state->context_ = this;
  state->id_ = static_cast<uint32_t>(held_.size());
  held_.push_back(state);
}

void Context::AddOwned(void* object, void (*destroy)(void*)) {
  owned_.push_back({object, destroy});
}

OperatorState* Context::Made(OperatorState* state) {
  SetHolds(*state, state->registry_ != nullptr);
  return state;
}

void Context::SetHolds(OperatorState& state, bool holds) {
  holding_ += static_cast<uint32_t>(holds) - state.holds_;
  state.holds_ = holds;
}

void Context::EndHeldRun(OperatorState& state) {
  const OperatorState::Registry& registry = *state.registry_;
  for (const OperatorState::HeldValue& value : registry.values) {
    switch (value.size) {
      case 1:
        memcpy(value.where, &value.initial, 1);
        break;
      case 4:
        memcpy(value.where, &value.initial, 4);
        break;
      default:
        memcpy(value.where, &value.initial, 8);
        break;
    }
  }
  for (const OperatorState::HeldObject& held : registry.held) {
    held.end_run(held.where);
  }
}

}  // namespace perfetto::trace_processor::core::exec
