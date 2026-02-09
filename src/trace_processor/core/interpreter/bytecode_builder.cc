/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/core/interpreter/bytecode_builder.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter {

uint32_t BytecodeBuilder::CreateCacheScope() {
  auto scope_id = static_cast<uint32_t>(scope_caches_.size());
  scope_caches_.emplace_back();
  return scope_id;
}

void BytecodeBuilder::ClearCacheScope(uint32_t scope_id) {
  PERFETTO_CHECK(scope_id < scope_caches_.size());
  scope_caches_[scope_id].Clear();
}

BytecodeBuilder::ScratchRegisters BytecodeBuilder::GetOrCreateScratchRegisters(
    uint32_t slot_id,
    uint32_t size) {
  if (slot_id >= scratch_slots_.size()) {
    scratch_slots_.resize(slot_id + 1);
  }
  auto& slot = scratch_slots_[slot_id];
  if (slot.has_value()) {
    PERFETTO_CHECK(size <= slot->size);
    PERFETTO_CHECK(!slot->in_use);
    return ScratchRegisters{slot->slab, slot->span};
  }
  auto slab = AllocateRegister<Slab<uint32_t>>();
  auto span = AllocateRegister<Span<uint32_t>>();
  slot = ScratchIndices{size, slab, span, false};
  return ScratchRegisters{slab, span};
}

BytecodeBuilder::ScratchRegisters BytecodeBuilder::AllocateScratch(
    uint32_t slot_id,
    uint32_t size) {
  auto regs = GetOrCreateScratchRegisters(slot_id, size);

  auto& alloc = AddOpcode<AllocateIndices>(Index<AllocateIndices>());
  alloc.arg<AllocateIndices::size>() = size;
  alloc.arg<AllocateIndices::dest_slab_register>() = regs.slab;
  alloc.arg<AllocateIndices::dest_span_register>() = regs.span;

  // GetOrCreateScratchRegisters guarantees the slot exists
  scratch_slots_[slot_id]->in_use = true;

  return regs;
}

void BytecodeBuilder::MarkScratchInUse(uint32_t slot_id) {
  PERFETTO_CHECK(slot_id < scratch_slots_.size() &&
                 scratch_slots_[slot_id].has_value());
  scratch_slots_[slot_id]->in_use = true;
}

void BytecodeBuilder::ReleaseScratch(uint32_t slot_id) {
  if (slot_id < scratch_slots_.size() && scratch_slots_[slot_id].has_value()) {
    scratch_slots_[slot_id]->in_use = false;
  }
}

Bytecode& BytecodeBuilder::AddRawOpcode(uint32_t option) {
  bytecode_.emplace_back();
  bytecode_.back().option = option;
  return bytecode_.back();
}

}  // namespace perfetto::trace_processor::core::interpreter
