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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_TYPES_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/type_set.h"

namespace perfetto::trace_processor::dataframe::impl {

// Type categories for column content and operations.
// These define which operations can be applied to which content types.

// Set of content types that aren't string-based.
using NonStringType = TypeSet<Id>;

// Set of content types that are numeric in nature.
using NumericType = TypeSet<Id>;

// Set of operations applicable to non-null values.
using NonNullOp = TypeSet<Eq>;

// Set of operations applicable to non-string values.
using NonStringOp = TypeSet<Eq>;

// Set of operations applicable to ranges.
using RangeOp = TypeSet<Eq>;

// Indicates an operation applies to both bounds of a range.
struct BothBounds {};
using BoundModifier = TypeSet<BothBounds>;

// Represents a range where both bounds are equal (point value).
struct EqualRange {};
using EqualRangeLowerBoundUpperBound = TypeSet<EqualRange>;

// Storage implementation for column data. Provides physical storage
// for different types of column content.
class Storage {
 public:
  // Storage representation for Id columns.
  struct Id {
    uint32_t size;  // Number of rows in the column
  };

  explicit Storage(Storage::Id data) : data_(data) {}

  // Type-safe access to storage with unchecked variant access.
  template <typename T>
  auto& unchecked_get() {
    using U = ColumnType::VariantTypeAtIndex<T, Variant>;
    return base::unchecked_get<U>(data_);
  }

  template <typename T>
  const auto& unchecked_get() const {
    using U = ColumnType::VariantTypeAtIndex<T, Variant>;
    return base::unchecked_get<U>(data_);
  }

  // Get raw pointer to storage data for a specific type.
  template <typename T>
  auto* unchecked_data() {
    return unchecked_get<T>().data();
  }

  template <typename T>
  const auto* unchecked_data() const {
    return unchecked_get<T>().data();
  }

 private:
  // Variant containing all possible storage representations.
  using Variant = std::variant<Id>;
  Variant data_;
};

// Provides overlay data for columns with special properties (e.g. nullability).
class Overlay {
 private:
  template <typename T>
  static constexpr uint32_t TypeIndex() {
    return base::variant_index<Variant, T>();
  }

 public:
  // No overlay data (for columns with default properties).
  struct NoOverlay {};

  explicit Overlay(NoOverlay n) : data_(n) {}

  // Type-safe unchecked access to variant data.
  template <typename T>
  T& uget() {
    return base::unchecked_get<T>(data_);
  }

  template <typename T>
  const T& uget() const {
    return base::unchecked_get<T>(data_);
  }

 private:
  // Variant containing all possible overlay types.
  using Variant = std::variant<NoOverlay>;
  Variant data_;
};

// Combines column specification with storage implementation.
// Represents a complete column in the dataframe.
struct Column {
  ColumnSpec spec;  // Column specifications (name, type, etc.)
  Storage storage;  // Physical storage for column data
  Overlay overlay;  // Optional overlay data for special properties
};

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
  using Value = std::variant<Id>;

  bool operator==(const CastFilterValueResult& other) const {
    return validity == other.validity && value == other.value;
  }

  static constexpr CastFilterValueResult Valid(Value value) {
    return CastFilterValueResult{Validity::kValid, value};
  }

  static constexpr CastFilterValueResult NoneMatch() {
    return CastFilterValueResult{Validity::kNoneMatch, Id{0}};
  }

  static constexpr CastFilterValueResult AllMatch() {
    return CastFilterValueResult{Validity::kAllMatch, Id{0}};
  }

  // Status of the casting result.
  Validity validity;

  // Variant of all possible cast value types.
  Value value;
};

// Represents a contiguous range of indices [b, e).
// Used for efficient representation of sequential row indices.
struct Range {
  uint32_t b;  // Beginning index (inclusive)
  uint32_t e;  // Ending index (exclusive)

  // Get the number of elements in the range.
  size_t size() const { return e - b; }
};

// Represents a contiguous sequence of elements of an arbitrary type T.
// Basically a very simple backport of std::span to C++17.
template <typename T>
struct Span {
  T* b;
  T* e;

  size_t size() const { return static_cast<size_t>(e - b); }
};

}  // namespace perfetto::trace_processor::dataframe::impl

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_TYPES_H_
