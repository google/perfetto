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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_IMPL_BYTECODE_INTERPRETER_OUTLINED_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_IMPL_BYTECODE_INTERPRETER_OUTLINED_H_

#include <cstdint>
#include <memory>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/slab.h"
#include "src/trace_processor/core/dataframe/impl/types.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode::outlined {

// Outlined implementation of SortRowLayout bytecode.
// Sorts indices based on row layout data in buffer.
void SortRowLayoutImpl(const Slab<uint8_t>& buffer,
                       uint32_t stride,
                       Span<uint32_t>& indices);

// Outlined implementation of FinalizeRanksInMap bytecode.
// Sorts string IDs and assigns ranks in the map.
void FinalizeRanksInMapImpl(
    const StringPool* string_pool,
    std::unique_ptr<base::FlatHashMap<StringPool::Id, uint32_t>>& rank_map);

// Outlined implementation of Distinct bytecode.
// Removes duplicate rows based on row layout data.
void DistinctImpl(const Slab<uint8_t>& buffer,
                  uint32_t stride,
                  Span<uint32_t>& indices);

// Outlined implementation of glob filtering for strings.
// Returns pointer past last written output index.
uint32_t* StringFilterGlobImpl(const StringPool* string_pool,
                               const StringPool::Id* data,
                               const char* pattern,
                               const uint32_t* begin,
                               const uint32_t* end,
                               uint32_t* output);

// Outlined implementation of regex filtering for strings.
// Returns pointer past last written output index.
uint32_t* StringFilterRegexImpl(const StringPool* string_pool,
                                const StringPool::Id* data,
                                const char* pattern,
                                const uint32_t* begin,
                                const uint32_t* end,
                                uint32_t* output);

}  // namespace perfetto::trace_processor::dataframe::impl::bytecode::outlined

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_IMPL_BYTECODE_INTERPRETER_OUTLINED_H_
