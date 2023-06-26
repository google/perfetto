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

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

using Range = RowMap::Range;

// A range of indices in the table space.
struct TableRange {
  TableRange(uint32_t start, uint32_t end) : range(start, end) {}
  explicit TableRange(Range r) : range(r) {}

  Range range;
};

// A range of indices in the storage space.
struct StorageRange {
  StorageRange(uint32_t start, uint32_t end) : range(start, end) {}
  explicit StorageRange(Range r) : range(r) {}

  Range range;
};

// A BitVector with set bits corresponding to indices in the table space.
struct TableBitVector {
  BitVector bv;
};

// A BitVector with set bits corresponding to indices in the table space.
struct StorageBitVector {
  BitVector bv;
};

// RangeOrBitVector of indices in the table space.
struct TableRangeOrBitVector {
  explicit TableRangeOrBitVector(Range range) : val(range) {}
  explicit TableRangeOrBitVector(BitVector bv) : val(std::move(bv)) {}
  explicit TableRangeOrBitVector(RangeOrBitVector r_or_bv)
      : val(std::move(r_or_bv)) {}

  bool IsRange() const { return val.IsRange(); }
  bool IsBitVector() const { return val.IsBitVector(); }

  BitVector TakeIfBitVector() && { return std::move(val).TakeIfBitVector(); }
  Range TakeIfRange() && { return std::move(val).TakeIfRange(); }

  RangeOrBitVector val = RangeOrBitVector(Range());
};

// Represents a vector of indices in the table space.
struct TableIndexVector {
  std::vector<uint32_t> indices;

  uint32_t size() const { return static_cast<uint32_t>(indices.size()); }
};

// Represents a vector of indices in the storage space.
struct StorageIndexVector {
  std::vector<uint32_t> indices;

  uint32_t size() const { return static_cast<uint32_t>(indices.size()); }
};

// A subset of FilterOp containing operations which can be handled by
// overlays.
enum class OverlayOp {
  kIsNull,
  kIsNotNull,
  kOther,
};

inline OverlayOp FilterOpToOverlayOp(FilterOp op) {
  if (op == FilterOp::kIsNull) {
    return OverlayOp::kIsNull;
  }
  if (op == FilterOp::kIsNotNull) {
    return OverlayOp::kIsNotNull;
  }
  return OverlayOp::kOther;
}

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
