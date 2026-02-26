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

#ifndef SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_BUILDER_H_
#define SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_BUILDER_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter {

// Low-level builder for bytecode instructions.
//
// This class provides generic bytecode building capabilities. It handles:
// - Register allocation
// - Scratch register management
// - Raw opcode emission
//
// Higher-level builders (like QueryPlanBuilder for dataframes or
// TreeTransformer for trees) use this class internally and add their own
// domain-specific logic.
class BytecodeBuilder {
 public:
  BytecodeBuilder() = default;

  // === Register allocation ===

  // Allocates a new register of type T and returns a read-write handle.
  template <typename T>
  RwHandle<T> AllocateRegister() {
    return RwHandle<T>{register_count_++};
  }

  // Returns the total number of registers allocated.
  uint32_t register_count() const { return register_count_; }

  // === Scratch register management ===
  //
  // Scratch slots provide reusable temporary storage. The allocator
  // dynamically finds the best-fit free slot (smallest slot with capacity
  // >= requested size), or creates a new one if none is available.

  // Result from AllocateScratch / GetOrCreateScratch.
  struct ScratchRegisters {
    RwHandle<Slab<uint32_t>> slab;
    RwHandle<Span<uint32_t>> span;
  };

  // Finds a free scratch slot with capacity >= |size| (best-fit) or creates
  // a new one. Emits AllocateIndices bytecode and marks the slot in-use.
  ScratchRegisters AllocateScratch(uint32_t size);

  // Finds a free scratch slot with capacity >= |size| (best-fit) or creates
  // a new one. Does NOT emit AllocateIndices — caller must emit it.
  // Marks the slot in-use.
  ScratchRegisters GetOrCreateScratch(uint32_t size);

  // Releases a scratch slot so it can be reused by future AllocateScratch
  // calls. Identified by matching slab/span registers.
  void ReleaseScratch(ScratchRegisters scratch);

  // === Opcode emission ===

  // Adds a new bytecode instruction of type T with the given option.
  // For simple bytecodes, use Index<T>() from bytecode_instructions.h.
  // For templated bytecodes, use Index<T>(params...) from
  // bytecode_instructions.h.
  template <typename T>
  T& AddOpcode(uint32_t option) {
    return static_cast<T&>(AddRawOpcode(option));
  }

  // Adds a raw bytecode with the given option value.
  Bytecode& AddRawOpcode(uint32_t option);

  // === Bytecode access ===

  BytecodeVector& bytecode() { return bytecode_; }
  const BytecodeVector& bytecode() const { return bytecode_; }

 private:
  // Scratch indices state.
  struct ScratchSlot {
    uint32_t size;
    RwHandle<Slab<uint32_t>> slab;
    RwHandle<Span<uint32_t>> span;
    bool in_use = false;
  };

  // Finds the best-fit free slot (smallest with capacity >= size), or
  // returns nullptr if none exists.
  ScratchSlot* FindBestFitFreeSlot(uint32_t size);

  BytecodeVector bytecode_;
  uint32_t register_count_ = 0;

  // Scratch management - dynamically allocated slots.
  std::vector<ScratchSlot> scratch_slots_;
};

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_BUILDER_H_
