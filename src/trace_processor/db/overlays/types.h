/*
 * Copyright (C) 2023 The Android Open Source Project
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
#ifndef SRC_TRACE_PROCESSOR_DB_OVERLAYS_TYPES_H_
#define SRC_TRACE_PROCESSOR_DB_OVERLAYS_TYPES_H_

#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

// A range of indices in the table space.
struct TableRange {
  RowMap::Range range;
};

// A range of indices in the storage space.
struct StorageRange {
  RowMap::Range range;
};

// A BitVector with set bits corresponding to indices in the table space.
struct TableBitVector {
  BitVector bv;
};

// A BitVector with set bits corresponding to indices in the table space.
struct StorageBitVector {
  BitVector bv;
};

// Represents a vector of indices in the table space.
struct TableIndexVector {
  std::vector<uint32_t> indices;
};

// Represents a vector of indices in the storage space.
struct StorageIndexVector {
  std::vector<uint32_t> indices;
};

// A subset of FilterOp containing operations which can be handled by
// overlays.
enum class OverlayOp {
  kIsNull,
  kIsNotNull,
  kOther,
};

// Contains estimates of the cost for each of method in this class per row.
struct CostEstimatePerRow {
  uint32_t to_storage_range;
  uint32_t to_table_bit_vector;
  uint32_t is_storage_search_required;
  uint32_t map_to_storage_index_vector;
  uint32_t index_search;
};

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_OVERLAYS_TYPES_H_
