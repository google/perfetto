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

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

Context::Context() = default;

Context::Slot::Slot() = default;
Context::Slot::~Slot() = default;

Context::~Context() {
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
    Restart(*entry.pipeline, *slot.pipeline);
  }
  slot.running = true;
  return slot;
}

base::StatusOr<OperatorState*> Context::BuildSink(uint32_t id) {
  if (slots_[id].built) {
    return slots_[id].state.get();
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
  return slot.state.get();
}

OperatorState* Context::IfBuilt(uint32_t id) const {
  return slots_[id].built ? slots_[id].state.get() : nullptr;
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
  slot.pipeline->EndRun();
  if (slot.state) {
    slot.state->EndRun();
  }
}

}  // namespace perfetto::trace_processor::core::exec
