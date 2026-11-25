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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {
struct Context::Held {
  struct Bytes {
    void* where;
    // A copy of the bytes as they were when the state was made.
    void* made;
    uint32_t size;
  };

  // The state and its size.
  void* state;
  uint32_t size;
  // Whether the state is a HeldState, whose members end their own runs.
  bool members = false;
  // The state's own bytes, if they are restored.
  std::vector<Bytes> bytes;
};

namespace {

// Blocks use Slab's alignment, so a kept block can be reused for any type.
constexpr size_t kBlockAlignment = Slab<uint8_t>::kAlignment;

}  // namespace

Context::Context() = default;

Context::~Context() {
  held_.clear();
  batches_.clear();
  for (const Owned& owned : owned_) {
    owned.destroy(owned.object);
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

base::StatusOr<OperatorState*> Context::BuildSink(uint32_t id) {
  if (slots_[id].built) {
    return slots_[id].state;
  }
  Slot& slot = Start(id);
  const Pipelines::Entry& entry = (*pipelines_)[id];
  const Pipeline& pipeline = *entry.pipeline;
  const Sink& sink = *entry.sink;
  base::Status status;
  while (RowBatch* batch = pipeline.Next(*slot.pipeline)) {
    status = sink.ConsumeBatch(*batch, *slot.state);
    if (!status.ok()) {
      break;
    }
  }
  if (status.ok()) {
    status = pipeline.status(*slot.pipeline);
  }
  if (status.ok()) {
    status = sink.FinalizeSink(*slot.state);
  }
  if (!status.ok()) {
    EndPipelineRun(id);
    return status;
  }
  slot.built = true;
  return slot.state;
}

OperatorState* Context::IfBuilt(uint32_t id) const {
  if (id >= slot_count_) {
    return nullptr;
  }
  return slots_[id].built ? slots_[id].state : nullptr;
}

RowBatch* Context::NextBatch(uint32_t id) {
  Slot& slot = slots_[id].running ? slots_[id] : Start(id);
  const Pipelines::Entry& entry = (*pipelines_)[id];
  return entry.pipeline->Next(*slot.pipeline);
}

base::Status Context::PipelineStatus(uint32_t id) const {
  const Slot& slot = slots_[id];
  if (!slot.pipeline) {
    return base::OkStatus();
  }
  const Pipelines::Entry& entry = (*pipelines_)[id];
  return entry.pipeline->status(*slot.pipeline);
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

void Context::AddState(OperatorState* state, uint32_t size, bool restored) {
  state->context_ = this;
  state->id_ = static_cast<uint32_t>(held_.size());
  Held& held = held_.emplace_back();
  held.state = state;
  held.size = size;
  if (restored) {
    // The fields of OperatorState itself never change, so they are not saved.
    static_assert(sizeof(OperatorState) ==
                  sizeof(void*) + 2 * sizeof(uint32_t));
    AddRunBytes(*state, reinterpret_cast<char*>(state) + sizeof(OperatorState),
                size - static_cast<uint32_t>(sizeof(OperatorState)));
  }
}

void Context::AddHeldState(OperatorState* state) {
  held_[state->id_].members = true;
}

void* Context::AllocateSpan(size_t size, size_t align) {
  return states_.Allocate(size,
                          size >= kBlockAlignment ? kBlockAlignment : align);
}

void Context::AddRunBytes(OperatorState& owner, void* where, uint32_t size) {
  if (size) {
    held_[owner.id_].bytes.push_back({where, nullptr, size});
  }
}

void Context::AddOwned(void* object, void (*destroy)(void*)) {
  owned_.push_back({object, destroy});
}

OperatorState* Context::Made(OperatorState* state) {
  Held& held = held_[state->id_];
  for (Held::Bytes& bytes : held.bytes) {
    bytes.made = states_.Allocate(bytes.size, alignof(std::max_align_t));
    memcpy(bytes.made, bytes.where, bytes.size);
  }
  SetHolds(*state, Holds(held, *state));
  return state;
}

void Context::SetHolds(OperatorState& state, bool holds) {
  holding_ += static_cast<uint32_t>(holds) - state.holds_;
  state.holds_ = holds;
}

bool Context::Holds(const Held& held, OperatorState& state) {
  return !held.bytes.empty() ||
         (held.members && static_cast<HeldState&>(state).holds());
}

void Context::EndHeldRun(OperatorState& state) {
  Held& held = held_[state.id_];
  if (held.members) {
    static_cast<HeldState&>(state).EndRun();
  }
  for (const Held::Bytes& bytes : held.bytes) {
    memcpy(bytes.where, bytes.made, bytes.size);
  }
}

RowBatch* Context::NewBatch() {
  return batches_.emplace_back(std::make_unique<RowBatch>()).get();
}
}  // namespace perfetto::trace_processor::core::exec
