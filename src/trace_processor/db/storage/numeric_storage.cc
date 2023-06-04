
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

#include "src/trace_processor/db/storage/numeric_storage.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

// All viable numeric values for ColumnTypes.
using NumericValue = std::variant<uint32_t, int32_t, int64_t, double_t>;

// Using the fact that binary operators in std are operators() of classes, we
// can wrap those classes in variants and use them for std::visit in
// SerialComparators. This helps prevent excess templating and switches.
template <typename T>
using FilterOpVariant = std::variant<std::greater<T>,
                                     std::greater_equal<T>,
                                     std::less<T>,
                                     std::less_equal<T>,
                                     std::equal_to<T>,
                                     std::not_equal_to<T>>;

// Based on SqlValue and ColumnType, casts SqlValue to proper type, returns
// std::nullopt if SqlValue can't be cast and should be considered invalid for
// comparison.
inline std::optional<NumericValue> GetNumericTypeVariant(ColumnType type,
                                                         SqlValue val) {
  if (val.is_null())
    return std::nullopt;

  switch (type) {
    case ColumnType::kDouble:
      return val.AsDouble();
    case ColumnType::kInt64:
      return val.AsLong();
    case ColumnType::kInt32:
      if (val.AsLong() > std::numeric_limits<int32_t>::max() ||
          val.AsLong() < std::numeric_limits<int32_t>::min())
        return std::nullopt;
      return static_cast<int32_t>(val.AsLong());
    case ColumnType::kUint32:
      if (val.AsLong() > std::numeric_limits<uint32_t>::max() ||
          val.AsLong() < std::numeric_limits<uint32_t>::min())
        return std::nullopt;
      return static_cast<uint32_t>(val.AsLong());
    case ColumnType::kString:
    case ColumnType::kDummy:
    case ColumnType::kId:
      return std::nullopt;
  }
  PERFETTO_FATAL("For GCC");
}

// Fetch std binary comparator class based on FilterOp. Can be used in
// std::visit for comparison.
template <typename T>
inline FilterOpVariant<T> GetFilterOpVariant(FilterOp op) {
  switch (op) {
    case FilterOp::kEq:
      return FilterOpVariant<T>(std::equal_to<T>());
    case FilterOp::kNe:
      return FilterOpVariant<T>(std::not_equal_to<T>());
    case FilterOp::kGe:
      return FilterOpVariant<T>(std::greater_equal<T>());
    case FilterOp::kGt:
      return FilterOpVariant<T>(std::greater<T>());
    case FilterOp::kLe:
      return FilterOpVariant<T>(std::less_equal<T>());
    case FilterOp::kLt:
      return FilterOpVariant<T>(std::less<T>());
    case FilterOp::kGlob:
    case FilterOp::kIsNotNull:
    case FilterOp::kIsNull:
      PERFETTO_FATAL("Not a valid operation on numeric type.");
  }
  PERFETTO_FATAL("For GCC");
}

uint32_t LowerBoundIntrinsic(const void* data,
                             NumericValue val,
                             RowMap::Range search_range) {
  return std::visit(
      [data, search_range](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto lower = std::lower_bound(typed_start + search_range.start,
                                      typed_start + search_range.end, val_data);
        return static_cast<uint32_t>(std::distance(typed_start, lower));
      },
      val);
}

uint32_t UpperBoundIntrinsic(const void* data,
                             NumericValue val,
                             RowMap::Range search_range) {
  return std::visit(
      [data, search_range](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto upper = std::upper_bound(typed_start + search_range.start,
                                      typed_start + search_range.end, val_data);
        return static_cast<uint32_t>(std::distance(typed_start, upper));
      },
      val);
}

uint32_t LowerBoundExtrinsic(const void* data,
                             NumericValue val,
                             uint32_t* indices,
                             uint32_t indices_count) {
  return std::visit(
      [data, indices, indices_count](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto lower =
            std::lower_bound(indices, indices + indices_count, val_data,
                             [typed_start](uint32_t index, T val) {
                               return typed_start[index] < val;
                             });
        return static_cast<uint32_t>(std::distance(indices, lower));
      },
      val);
}

uint32_t UpperBoundExtrinsic(const void* data,
                             NumericValue val,
                             uint32_t* indices,
                             uint32_t indices_count) {
  return std::visit(
      [data, indices, indices_count](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto upper =
            std::upper_bound(indices, indices + indices_count, val_data,
                             [typed_start](T val, uint32_t index) {
                               return val < typed_start[index];
                             });
        return static_cast<uint32_t>(std::distance(indices, upper));
      },
      val);
}

template <typename T, typename Comparator>
void TypedLinearSearch(T typed_val,
                       const T* start,
                       Comparator comparator,
                       BitVector::Builder& builder) {
  // Slow path: we compare <64 elements and append to get us to a word
  // boundary.
  const T* ptr = start;
  uint32_t front_elements = builder.BitsUntilWordBoundaryOrFull();
  for (uint32_t i = 0; i < front_elements; ++i) {
    builder.Append(comparator(ptr[i], typed_val));
  }
  ptr += front_elements;

  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
    uint64_t word = 0;
    // This part should be optimised by SIMD and is expected to be fast.
    for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k) {
      bool comp_result = comparator(start[i + k], typed_val);
      word |= static_cast<uint64_t>(comp_result) << k;
    }
    builder.AppendWord(word);
  }
  ptr += fast_path_elements;

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  for (uint32_t i = 0; i < back_elements; ++i) {
    builder.Append(comparator(ptr[i], typed_val));
  }
}

template <typename T, typename Comparator>
void TypedIndexSearch(T typed_val,
                      const T* start,
                      uint32_t* indices,
                      Comparator comparator,
                      BitVector::Builder& builder) {
  // Slow path: we compare <64 elements and append to get us to a word
  // boundary.
  const T* ptr = start;
  uint32_t front_elements = builder.BitsUntilWordBoundaryOrFull();
  for (uint32_t i = 0; i < front_elements; ++i) {
    builder.Append(comparator(ptr[indices[i]], typed_val));
  }
  ptr += front_elements;

  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
    uint64_t word = 0;
    // This part should be optimised by SIMD and is expected to be fast.
    for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k) {
      bool comp_result = comparator(start[indices[i + k]], typed_val);
      word |= static_cast<uint64_t>(comp_result) << k;
    }
    builder.AppendWord(word);
  }
  ptr += fast_path_elements;

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  for (uint32_t i = 0; i < back_elements; ++i) {
    builder.Append(comparator(ptr[indices[i]], typed_val));
  }
}

}  // namespace

BitVector NumericStorage::LinearSearch(FilterOp op,
                                       SqlValue sql_val,
                                       RowMap::Range range) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (op == FilterOp::kIsNotNull)
    return BitVector(size(), true);

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return BitVector(size(), false);

  BitVector::Builder builder(range.end);
  builder.Skip(range.start);
  std::visit(
      [this, range, op, &builder](auto val) {
        using T = decltype(val);
        auto* start = static_cast<const T*>(data_) + range.start;
        std::visit(
            [start, val, &builder](auto comparator) {
              TypedLinearSearch(val, start, comparator, builder);
            },
            GetFilterOpVariant<T>(op));
      },
      *val);
  return std::move(builder).Build();
}

BitVector NumericStorage::IndexSearch(FilterOp op,
                                      SqlValue sql_val,
                                      uint32_t* indices,
                                      uint32_t indices_count) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (op == FilterOp::kIsNotNull)
    return BitVector(size(), true);

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return BitVector(size(), false);

  BitVector::Builder builder(indices_count);
  std::visit(
      [this, indices, op, &builder](auto val) {
        using T = decltype(val);
        auto* start = static_cast<const T*>(data_);
        std::visit(
            [start, indices, val, &builder](auto comparator) {
              TypedIndexSearch(val, start, indices, comparator, builder);
            },
            GetFilterOpVariant<T>(op));
      },
      *val);
  return std::move(builder).Build();
}

RowMap::Range NumericStorage::BinarySearchIntrinsic(
    FilterOp op,
    SqlValue sql_val,
    RowMap::Range search_range) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (op == FilterOp::kIsNotNull)
    return RowMap::Range(0, size());

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return RowMap::Range();

  switch (op) {
    case FilterOp::kEq:
      return RowMap::Range(LowerBoundIntrinsic(data_, *val, search_range),
                           UpperBoundIntrinsic(data_, *val, search_range));
    case FilterOp::kLe:
      return RowMap::Range(0, UpperBoundIntrinsic(data_, *val, search_range));
    case FilterOp::kLt:
      return RowMap::Range(0, LowerBoundIntrinsic(data_, *val, search_range));
    case FilterOp::kGe:
      return RowMap::Range(LowerBoundIntrinsic(data_, *val, search_range),
                           size_);
    case FilterOp::kGt:
      return RowMap::Range(UpperBoundIntrinsic(data_, *val, search_range),
                           size_);
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
      return RowMap::Range();
  }
  return RowMap::Range();
}

RowMap::Range NumericStorage::BinarySearchExtrinsic(
    FilterOp op,
    SqlValue sql_val,
    uint32_t* indices,
    uint32_t indices_count) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);

  if (op == FilterOp::kIsNotNull)
    return RowMap::Range(0, size());

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return RowMap::Range();

  switch (op) {
    case FilterOp::kEq:
      return RowMap::Range(
          LowerBoundExtrinsic(data_, *val, indices, indices_count),
          UpperBoundExtrinsic(data_, *val, indices, indices_count));
    case FilterOp::kLe:
      return RowMap::Range(
          0, UpperBoundExtrinsic(data_, *val, indices, indices_count));
    case FilterOp::kLt:
      return RowMap::Range(
          0, LowerBoundExtrinsic(data_, *val, indices, indices_count));
    case FilterOp::kGe:
      return RowMap::Range(
          LowerBoundExtrinsic(data_, *val, indices, indices_count), size_);
    case FilterOp::kGt:
      return RowMap::Range(
          UpperBoundExtrinsic(data_, *val, indices, indices_count), size_);
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
      return RowMap::Range();
  }
  return RowMap::Range();
}

void NumericStorage::StableSort(uint32_t* rows, uint32_t rows_size) const {
  NumericValue val = *GetNumericTypeVariant(type_, SqlValue::Long(0));
  std::visit(
      [this, &rows, rows_size](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        std::stable_sort(rows, rows + rows_size,
                         [typed_start](uint32_t a_idx, uint32_t b_idx) {
                           T first_val = typed_start[a_idx];
                           T second_val = typed_start[b_idx];
                           return first_val < second_val;
                         });
      },
      val);
}

void NumericStorage::Sort(uint32_t*, uint32_t) const {}

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
