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
#include "perfetto/ext/base/type_set.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::interpreter {

// Indicates an operation applies to both bounds of a range.
struct BothBounds {};

// Indicates an operation applies to the lower bound of a range.
struct BeginBound {};

// Indicates an operation applies to the upper bound of a range.
struct EndBound {};

// Which bounds should be modified by a range operation.
using BoundModifier = base::TypeSet<BothBounds, BeginBound, EndBound>;

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
    base::TypeSet<EqualRange, LowerBound, UpperBound>;

// Type tag indicating nulls should be placed at the start during
// partitioning/sorting.
struct NullsAtStart {};

// Type tag indicating nulls should be placed at the end during
// partitioning/sorting.
struct NullsAtEnd {};

// TypeSet defining the possible placement locations for nulls.
using NullsLocation = base::TypeSet<NullsAtStart, NullsAtEnd>;

// Type tag for finding the minimum value.
struct MinOp {};

// Type tag for finding the maximum value.
struct MaxOp {};

// TypeSet combining Min and Max operations.
using MinMaxOp = base::TypeSet<MinOp, MaxOp>;

// TypeSet which collapses all of the sparse nullability types into a single
// type.
using SparseNullCollapsedNullability =
    base::TypeSet<NonNull, SparseNull, DenseNull>;

// Handle for referring to a filter value during query execution.
struct FilterValueHandle {
  uint32_t index;  // Index into the filter value array
};

using filter::CastFilterValueListResult;
using filter::CastFilterValueResult;

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_INTERPRETER_TYPES_H_
