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
#include <limits>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter {

BytecodeBuilder::ScratchSlot* BytecodeBuilder::FindBestFitFreeSlot(
    uint32_t size) {
  ScratchSlot* best = nullptr;
  uint32_t best_size = std::numeric_limits<uint32_t>::max();
  for (auto& slot : scratch_slots_) {
    if (!slot.in_use && slot.size >= size && slot.size < best_size) {
      best = &slot;
      best_size = slot.size;
    }
  }
  return best;
}

BytecodeBuilder::ScratchRegisters BytecodeBuilder::GetOrCreateScratch(
    uint32_t size) {
  if (auto* slot = FindBestFitFreeSlot(size)) {
    slot->in_use = true;
    return {slot->slab, slot->span};
  }
  auto slab = AllocateRegister<Slab<uint32_t>>();
  auto span = AllocateRegister<Span<uint32_t>>();
  scratch_slots_.push_back({size, slab, span, true});
  return {slab, span};
}

BytecodeBuilder::ScratchRegisters BytecodeBuilder::AllocateScratch(
    uint32_t size) {
  auto regs = GetOrCreateScratch(size);

  auto& alloc = AddOpcode<AllocateIndices>(Index<AllocateIndices>());
  alloc.arg<AllocateIndices::size>() = size;
  alloc.arg<AllocateIndices::dest_slab_register>() = regs.slab;
  alloc.arg<AllocateIndices::dest_span_register>() = regs.span;

  return regs;
}

void BytecodeBuilder::ReleaseScratch(ScratchRegisters scratch) {
  for (auto& slot : scratch_slots_) {
    if (slot.slab.index == scratch.slab.index &&
        slot.span.index == scratch.span.index) {
      PERFETTO_DCHECK(slot.in_use);
      slot.in_use = false;
      return;
    }
  }
  PERFETTO_DFATAL("ReleaseScratch: no matching slot found");
}

Bytecode& BytecodeBuilder::AddRawOpcode(uint32_t option) {
  bytecode_.emplace_back();
  bytecode_.back().option = option;
  return bytecode_.back();
}

}  // namespace perfetto::trace_processor::core::interpreter
