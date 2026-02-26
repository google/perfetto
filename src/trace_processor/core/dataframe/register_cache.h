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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_REGISTER_CACHE_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_REGISTER_CACHE_H_

#include <cstdint>
#include <utility>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"

namespace perfetto::trace_processor::core::dataframe {

// Register type identifiers for cache key encoding.
enum RegType : uint32_t {
  kStorageReg = 0,
  kNullBvReg = 1,
  kPrefixPopcountReg = 2,
  kSmallValueEqBvReg = 3,
  kSmallValueEqPopcountReg = 4,
  kIndexReg = 5,
  kRegTypeCount = 6,
};

// Cache for register handles, keyed by data source pointer (e.g. Column*,
// Index*) and register type.
//
// Used by QueryPlanBuilder for deduplication and by DataframeTransformer for
// tracking registers across operations.
//
// Using pointers as keys provides natural scoping: when Column objects change
// (e.g. after gather creates new Column instances), the new pointers miss the
// cache and fresh registers are allocated.
class RegisterCache {
 public:
  RegisterCache() = default;
  explicit RegisterCache(interpreter::BytecodeBuilder* builder)
      : builder_(builder) {}

  // Gets a register from cache, or allocates a new one.
  // |key| is typically Column* or Index*.
  // |reg_type| distinguishes different register kinds for the same key.
  // Returns the register and whether it was newly allocated.
  template <typename T>
  std::pair<interpreter::RwHandle<T>, bool> GetOrAllocate(const void* key,
                                                          RegType reg_type) {
    uintptr_t cache_key = MakeKey(key, reg_type);
    auto* it = cache_.Find(cache_key);
    if (it) {
      return {interpreter::RwHandle<T>{it->index}, false};
    }
    auto reg = builder_->AllocateRegister<T>();
    cache_[cache_key] = interpreter::HandleBase{reg.index};
    return {reg, true};
  }

  // Sets a register in the cache for the given key and type.
  void Set(const void* key, RegType reg_type, interpreter::HandleBase handle) {
    cache_[MakeKey(key, reg_type)] = handle;
  }

 private:
  // Combines pointer and reg_type into a single key. Since Column/Index
  // objects are much larger than kRegTypeCount, adding reg_type to the
  // pointer address produces unique keys without collision.
  static uintptr_t MakeKey(const void* ptr, uint32_t reg_type) {
    return reinterpret_cast<uintptr_t>(ptr) + reg_type;
  }

  interpreter::BytecodeBuilder* builder_ = nullptr;
  base::FlatHashMap<uintptr_t, interpreter::HandleBase> cache_;
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_REGISTER_CACHE_H_
