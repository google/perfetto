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

#include "perfetto/trace_processor/basic_types.h"

namespace perfetto {
namespace trace_processor {

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
