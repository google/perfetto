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

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter {

// Low-level builder for bytecode instructions.
//
// This class provides generic bytecode building capabilities. It handles:
// - Register allocation
// - Scope-based register caching (generic mechanism for callers to cache
//   registers within a scope)
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

  // === Scope-based register caching ===

  // Creates a new cache scope and returns its ID.
  // Scopes allow callers to cache registers and retrieve them later by
  // (reg_type, index) pairs.
  uint32_t CreateCacheScope();

  // Result from GetOrAllocateCachedRegister.
  template <typename T>
  struct CachedRegister {
    RwHandle<T> reg;
    bool inserted;  // True if newly allocated, false if found in cache
  };

  // Gets a register from the scope cache, or allocates a new one if not found.
  // The allocated register is automatically added to the cache.
  // Returns the register and whether it was newly inserted.
  template <typename T>
  CachedRegister<T> GetOrAllocateCachedRegister(uint32_t scope_id,
                                                uint32_t reg_type,
                                                uint32_t index) {
    if (scope_id >= scope_caches_.size()) {
      scope_caches_.resize(scope_id + 1);
    }
    uint64_t key = CacheKey(reg_type, index);
    auto* it = scope_caches_[scope_id].Find(key);
    if (it) {
      return {RwHandle<T>{it->index}, false};
    }
    auto reg = AllocateRegister<T>();
    scope_caches_[scope_id][key] = HandleBase{reg.index};
    return {reg, true};
  }

  // Clears all cached registers for a scope.
  void ClearCacheScope(uint32_t scope_id);

  // === Scratch register management ===
  //
  // These methods manage scratch register state for operations that need
  // temporary storage. The caller is responsible for emitting the actual
  // AllocateIndices opcode (this allows different cost tracking strategies).
  //
  // Multiple named scratch slots are supported via slot_id parameter.
  // The default slot (used by single-argument methods) is slot 0.

  // Result from GetOrCreateScratchRegisters.
  struct ScratchRegisters {
    RwHandle<Slab<uint32_t>> slab;
    RwHandle<Span<uint32_t>> span;
  };

  // Gets or creates scratch registers of the given size in the specified slot.
  // Multiple slots can coexist with different slot_ids.
  // Does NOT emit AllocateIndices - caller must emit it separately.
  ScratchRegisters GetOrCreateScratchRegisters(uint32_t slot_id, uint32_t size);

  // Gets or creates scratch registers in the default slot (slot 0).
  ScratchRegisters GetOrCreateScratchRegisters(uint32_t size) {
    return GetOrCreateScratchRegisters(0, size);
  }

  // Allocates scratch in the specified slot and emits AllocateIndices bytecode.
  // This is the preferred method - combines register allocation + bytecode
  // emission.
  ScratchRegisters AllocateScratch(uint32_t slot_id, uint32_t size);

  // Marks the specified scratch slot as being in use.
  void MarkScratchInUse(uint32_t slot_id);

  // Marks the default scratch slot (slot 0) as being in use.
  void MarkScratchInUse() { MarkScratchInUse(0); }

  // Releases the specified scratch slot so it can be reused.
  void ReleaseScratch(uint32_t slot_id);

  // Releases the default scratch slot (slot 0).
  void ReleaseScratch() { ReleaseScratch(0); }

  // Returns true if the specified scratch slot is currently in use.
  bool IsScratchInUse(uint32_t slot_id) const {
    return slot_id < scratch_slots_.size() &&
           scratch_slots_[slot_id].has_value() &&
           scratch_slots_[slot_id]->in_use;
  }

  // Returns true if the default scratch slot (slot 0) is currently in use.
  bool IsScratchInUse() const { return IsScratchInUse(0); }

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
  struct ScratchIndices {
    uint32_t size;
    RwHandle<Slab<uint32_t>> slab;
    RwHandle<Span<uint32_t>> span;
    bool in_use = false;
  };

  // Combines reg_type and index into a single cache key.
  static constexpr uint64_t CacheKey(uint32_t reg_type, uint32_t index) {
    return (static_cast<uint64_t>(reg_type) << 32) | index;
  }

  BytecodeVector bytecode_;
  uint32_t register_count_ = 0;

  // Scope-based cache: scope_id -> (reg_type, index) -> register handle
  std::vector<base::FlatHashMap<uint64_t, HandleBase>> scope_caches_;

  // Scratch management - multiple slots indexed by slot_id
  std::vector<std::optional<ScratchIndices>> scratch_slots_;
};

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_BUILDER_H_
