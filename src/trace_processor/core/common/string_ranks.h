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

#ifndef SRC_TRACE_PROCESSOR_CORE_COMMON_STRING_RANKS_H_
#define SRC_TRACE_PROCESSOR_CORE_COMMON_STRING_RANKS_H_

#include <cstddef>
#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::core {

// Ranks strings by their contents.
class StringRanks {
 public:
  void Add(StringPool::Id id) { ranks_.Insert(id, 0); }

  // Sorts the string IDs added and assigns each its rank.
  void Rank(const StringPool& pool);

  const uint32_t* Find(StringPool::Id id) const { return ranks_.Find(id); }

  size_t size() const { return ranks_.size(); }
  void Clear() { ranks_.Clear(); }

 private:
  base::FlatHashMapV2<StringPool::Id, uint32_t> ranks_;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_STRING_RANKS_H_
