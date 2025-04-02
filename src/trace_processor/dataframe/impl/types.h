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
#include <utility>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bit_vector.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/type_set.h"

namespace perfetto::trace_processor::dataframe::impl {

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

// Storage implementation for column data. Provides physical storage
// for different types of column content.
class Storage {
 public:
  // Storage representation for Id columns.
  struct Id {
    uint32_t size;  // Number of rows in the column
  };
  using Uint32 = FlexVector<uint32_t>;
  using Int32 = FlexVector<int32_t>;
  using Int64 = FlexVector<int64_t>;
  using Double = FlexVector<double>;
  using String = FlexVector<StringPool::Id>;

  explicit Storage(Storage::Id data) : data_(data) {}
  explicit Storage(Storage::Uint32 data) : data_(std::move(data)) {}
  explicit Storage(Storage::Int32 data) : data_(std::move(data)) {}
  explicit Storage(Storage::Int64 data) : data_(std::move(data)) {}
  explicit Storage(Storage::Double data) : data_(std::move(data)) {}
  explicit Storage(Storage::String data) : data_(std::move(data)) {}

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
  using Variant = std::variant<Id, Uint32, Int32, Int64, Double, String>;
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

  // Sparse null overlay data (for columns with sparse NULL values).
  struct SparseNull {
    BitVector bit_vector;
  };

  // Dense null overlay data (for columns with dense NULL values).
  struct DenseNull {
    BitVector bit_vector;
  };

  explicit Overlay(NoOverlay n) : data_(n) {}
  explicit Overlay(SparseNull s) : data_(std::move(s)) {}
  explicit Overlay(DenseNull d) : data_(std::move(d)) {}

  // Type-safe unchecked access to variant data.
  template <typename T>
  T& unchecked_get() {
    return base::unchecked_get<T>(data_);
  }

  template <typename T>
  const T& unchecked_get() const {
    return base::unchecked_get<T>(data_);
  }

  BitVector& GetNullBitVector() {
    switch (data_.index()) {
      case TypeIndex<SparseNull>():
        return unchecked_get<SparseNull>().bit_vector;
      case TypeIndex<DenseNull>():
        return unchecked_get<DenseNull>().bit_vector;
      default:
        PERFETTO_FATAL("Unsupported overlay type");
    }
  }
  const BitVector& GetNullBitVector() const {
    switch (data_.index()) {
      case TypeIndex<SparseNull>():
        return unchecked_get<SparseNull>().bit_vector;
      case TypeIndex<DenseNull>():
        return unchecked_get<DenseNull>().bit_vector;
      default:
        PERFETTO_FATAL("Unsupported overlay type");
    }
  }

 private:
  // Variant containing all possible overlay types.
  using Variant = std::variant<NoOverlay, SparseNull, DenseNull>;
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
  using Value =
      std::variant<Id, uint32_t, int32_t, int64_t, double, const char*>;

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
  bool empty() const { return b == e; }
};

// Represents a contiguous sequence of elements of an arbitrary type T.
// Basically a very simple backport of std::span to C++17.
template <typename T>
struct Span {
  using value_type = T;
  using const_iterator = T*;

  T* b;
  T* e;

  Span(T* _b, T* _e) : b(_b), e(_e) {}

  T* begin() const { return b; }
  T* end() const { return e; }
  size_t size() const { return static_cast<size_t>(e - b); }
  bool empty() const { return b == e; }
};

}  // namespace perfetto::trace_processor::dataframe::impl

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_TYPES_H_
