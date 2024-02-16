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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_TYPES_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_TYPES_H_

#include <cstdint>
#include <utility>
#include <variant>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"

namespace perfetto::trace_processor {

using Range = RowMap::Range;

// Result of calling Storage::SingleSearch function.
enum class SingleSearchResult {
  kMatch,            // The specified row matches the constraint.
  kNoMatch,          // The specified row does not matches the constraint.
  kNeedsFullSearch,  // SingleSearch was unable to determine if the row meets
                     // the crtiteria, a call to *Search is required.
};

// Result of calling Storage::UniqueSearch function.
enum class UniqueSearchResult {
  kMatch,            // The returned row matches the constraint.
  kNoMatch,          // The returned row does not matches the constraint.
  kNeedsFullSearch,  // UniqueSearch was unable to determine if a row meets
                     // the crtiteria, a call to *Search is required. This
                     // does not mean there >1 row necessarily, just that
                     // UniqueSearch was unable to quickly identify a single
                     // row.
};

// Result of calling Storage::ValidateSearchResult function.
enum class SearchValidationResult {
  kOk,       // It makes sense to run search
  kAllData,  // Don't run search, all data passes the constraint.
  kNoData    // Don't run search, no data passes the constraint.
};

// Used for result of filtering, which is sometimes (for more optimised
// operations) a Range and BitVector otherwise. Stores a variant of Range and
// BitVector.
class RangeOrBitVector {
 public:
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
    return *std::get_if<Range>(&val);
  }

 private:
  std::variant<Range, BitVector> val = Range();
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

// Index vector related data required to Filter using IndexSearch.
struct Indices {
  enum class State {
    // We can't guarantee that data is in monotonic order.
    kNonmonotonic,
    // Data is in monotonic order.
    // TODO(b/307482437): Use this to optimise filtering if storage is sorted.
    kMonotonic
  };

  const uint32_t* data = nullptr;
  uint32_t size = 0;
  State state = Indices::State::kNonmonotonic;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_TYPES_H_
