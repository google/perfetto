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
#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_TYPES_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_TYPES_H_

#include <variant>
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/row_map.h"

namespace perfetto {
namespace trace_processor {

// Used for result of filtering, which is sometimes (for more optimised
// operations) a Range and BitVector otherwise. Stores a variant of Range and
// BitVector.
struct RangeOrBitVector {
  using Range = RowMap::Range;
  explicit RangeOrBitVector(Range range) : val(range) {}
  explicit RangeOrBitVector(BitVector bv) : val(std::move(bv)) {}

  bool IsRange() const { return std::holds_alternative<Range>(val); }
  bool IsBitVector() const { return std::holds_alternative<BitVector>(val); }

  BitVector TakeIfBitVector() && {
    PERFETTO_DCHECK(IsBitVector());
    return std::move(*std::get_if<BitVector>(&val));
  }
  Range TakeIfRange() && {
    PERFETTO_DCHECK(IsRange());
    return std::move(*std::get_if<Range>(&val));
  }

  std::variant<RowMap::Range, BitVector> val = Range();
};

// Represents the possible filter operations on a column.
enum class FilterOp {
  kEq,
  kNe,
  kGt,
  kLt,
  kGe,
  kLe,
  kIsNull,
  kIsNotNull,
  kGlob,
  kRegex,
};

// Represents a constraint on a column.
struct Constraint {
  uint32_t col_idx;
  FilterOp op;
  SqlValue value;
};

// Represents an order by operation on a column.
struct Order {
  uint32_t col_idx;
  bool desc;
};

// The enum type of the column.
// Public only to stop GCC complaining about templates being defined in a
// non-namespace scope (see ColumnTypeHelper below).
enum class ColumnType {
  // Standard primitive types.
  kInt32,
  kUint32,
  kInt64,
  kDouble,
  kString,

  // Types generated on the fly.
  kId,

  // Types which don't have any data backing them.
  kDummy,
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_TYPES_H_
