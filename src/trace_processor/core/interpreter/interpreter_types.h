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

#ifndef SRC_TRACE_PROCESSOR_CORE_INTERPRETER_INTERPRETER_TYPES_H_
#define SRC_TRACE_PROCESSOR_CORE_INTERPRETER_INTERPRETER_TYPES_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/type_set.h"

namespace perfetto::trace_processor::core::interpreter {

// Type categories for column content and operations.
// These define which operations can be applied to which content types.

// Set of content types that aren't string-based.
using NonStringType = TypeSet<Id, Uint32, Int32, Int64, Double>;

// Set of content types that are numeric in nature.
using IntegerOrDoubleType = TypeSet<Uint32, Int32, Int64, Double>;

// Set of operations applicable to non-null values.
using NonNullOp = TypeSet<Eq, Ne, Lt, Le, Gt, Ge, Glob, Regex>;

// Set of operations applicable to non-string values.
using NonStringOp = TypeSet<Eq, Ne, Lt, Le, Gt, Ge>;

// Set of operations applicable to string values.
using StringOp = TypeSet<Eq, Ne, Lt, Le, Gt, Ge, Glob, Regex>;

// Set of operations applicable to only string values.
using OnlyStringOp = TypeSet<Glob, Regex>;

// Set of operations applicable to ranges.
using RangeOp = TypeSet<Eq, Lt, Le, Gt, Ge>;

// Set of inequality operations (Lt, Le, Gt, Ge).
using InequalityOp = TypeSet<Lt, Le, Gt, Ge>;

// Set of null operations (IsNotNull, IsNull).
using NullOp = TypeSet<IsNotNull, IsNull>;

// Indicates an operation applies to both bounds of a range.
struct BothBounds {};

// Indicates an operation applies to the lower bound of a range.
struct BeginBound {};

// Indicates an operation applies to the upper bound of a range.
struct EndBound {};

// Which bounds should be modified by a range operation.
using BoundModifier = TypeSet<BothBounds, BeginBound, EndBound>;

// Represents a filter operation where we are performing an equality operation
// on a sorted column.
struct EqualRange {};

// Represents a filter operation where we are performing a lower bound operation
// on a sorted column.
struct LowerBound {};

// Represents a filter operation where we are performing an upper bound
// operation on a sorted column.
struct UpperBound {};

// Set of operations that can be applied to a sorted column.
using EqualRangeLowerBoundUpperBound =
    TypeSet<EqualRange, LowerBound, UpperBound>;

// Type tag indicating nulls should be placed at the start during
// partitioning/sorting.
struct NullsAtStart {};

// Type tag indicating nulls should be placed at the end during
// partitioning/sorting.
struct NullsAtEnd {};

// TypeSet defining the possible placement locations for nulls.
using NullsLocation = TypeSet<NullsAtStart, NullsAtEnd>;

// Type tag for finding the minimum value.
struct MinOp {};

// Type tag for finding the maximum value.
struct MaxOp {};

// TypeSet combining Min and Max operations.
using MinMaxOp = TypeSet<MinOp, MaxOp>;

// TypeSet containing all the non-id storage types.
using NonIdStorageType = TypeSet<Uint32, Int32, Int64, Double, String>;

// TypeSet which collapses all of the sparse nullability types into a single
// type.
using SparseNullCollapsedNullability = TypeSet<NonNull, SparseNull, DenseNull>;

// Handle for referring to a filter value during query execution.
struct FilterValueHandle {
  uint32_t index;  // Index into the filter value array
};

// Result of casting a filter value for comparison during query execution.
struct CastFilterValueResult {
  enum Validity : uint8_t { kValid, kAllMatch, kNoneMatch };

  // Cast value for Id columns.
  struct Id {
    bool operator==(const Id& other) const { return value == other.value; }
    uint32_t value;
  };
  using Value =
      std::variant<Id, uint32_t, int32_t, int64_t, double, const char*>;

  bool operator==(const CastFilterValueResult& other) const {
    return validity == other.validity && value == other.value;
  }

  static CastFilterValueResult Valid(Value value) {
    return CastFilterValueResult{Validity::kValid, std::move(value)};
  }
  static CastFilterValueResult NoneMatch() {
    return CastFilterValueResult{Validity::kNoneMatch, Id{0}};
  }
  static CastFilterValueResult AllMatch() {
    return CastFilterValueResult{Validity::kAllMatch, Id{0}};
  }

  // Status of the casting result.
  Validity validity;

  // Variant of all possible cast value types.
  Value value;
};

// Result of an operation that yields multiple values (e.g. from an IN clause).
struct CastFilterValueListResult {
  using Value = std::variant<CastFilterValueResult::Id,
                             uint32_t,
                             int32_t,
                             int64_t,
                             double,
                             StringPool::Id>;
  using ValueList = std::variant<FlexVector<CastFilterValueResult::Id>,
                                 FlexVector<uint32_t>,
                                 FlexVector<int32_t>,
                                 FlexVector<int64_t>,
                                 FlexVector<double>,
                                 FlexVector<StringPool::Id>>;
  using HashLookup = base::FlatHashMapV2<int64_t, bool>;

  // Optimized lookup for the non-indexed In filter. For dense Id/Uint32, a
  // BitVector. For large sparse integer/string lists, a HashLookup. For small
  // lists or when no optimized structure applies, this is std::monostate and
  // the non-indexed In bytecode falls back to linear scan over value_list.
  using Lookup = std::variant<std::monostate, BitVector, HashLookup>;

  using Ptr = std::unique_ptr<CastFilterValueListResult>;

  static Ptr Valid(ValueList vl, Lookup l) {
    return Ptr(new CastFilterValueListResult{
        CastFilterValueResult::Validity::kValid, std::move(vl), std::move(l)});
  }
  static Ptr Valid(ValueList vl) {
    return Ptr(
        new CastFilterValueListResult{CastFilterValueResult::Validity::kValid,
                                      std::move(vl), std::monostate{}});
  }
  static Ptr NoneMatch() {
    return Ptr(new CastFilterValueListResult{
        CastFilterValueResult::Validity::kNoneMatch, ValueList{},
        std::monostate{}});
  }
  static Ptr AllMatch() {
    return Ptr(new CastFilterValueListResult{
        CastFilterValueResult::Validity::kAllMatch, ValueList{},
        std::monostate{}});
  }

  CastFilterValueResult::Validity validity;

  // Always-present value list. Used by IndexedFilterIn (for binary search
  // iteration) and by the non-indexed In bytecode (for linear scan on
  // small lists).
  ValueList value_list;

  // Optional optimized lookup structure for the non-indexed In filter.
  // std::monostate when no optimization applies (small lists).
  Lookup lookup;
};

// Opaque state for tree operations. Bundles tree structure, column data
// copies, P2C cache, and scratch buffers into a single object that
// bytecodes modify in-place.
//
// Column data and null bitvectors are registered by the TreeTransformer
// and compacted alongside the tree structure by FilterTreeState.
struct TreeState {
  // Tree structure.
  Slab<uint32_t> parent;  // parent[i] = row index (kNullParent for roots)
  Slab<uint32_t> original_rows;  // original_rows[i] = original df row index
  uint32_t row_count = 0;

  // Column data copies (compacted alongside parent by FilterTreeState).
  struct ColumnStorage {
    Slab<uint8_t> data;
    uint32_t elem_size;
  };
  std::vector<ColumnStorage> columns;

  // Null bitvector copies (compacted alongside parent by FilterTreeState).
  std::vector<BitVector> null_bitvectors;

  // Reusable keep bitvector (allocated once at max size, cleared each use).
  BitVector keep_bv;

  // P2C CSR cache (rebuilt lazily).
  Slab<uint32_t> p2c_offsets;
  Slab<uint32_t> p2c_children;
  Slab<uint32_t> p2c_roots;
  uint32_t p2c_root_count = 0;
  bool p2c_valid = false;

  // Scratch buffers (allocated once at max size, reused).
  Slab<uint32_t> scratch1;  // initial_row_count * 2
  Slab<uint32_t> scratch2;  // initial_row_count
};

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_INTERPRETER_TYPES_H_
