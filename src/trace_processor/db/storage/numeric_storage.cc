
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
#include <string>
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/types.h"
#include "src/trace_processor/db/storage/utils.h"
#include "src/trace_processor/tp_metatrace.h"

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
    case FilterOp::kRegex:
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

template <typename T>
void TypedLinearSearch(T typed_val,
                       const T* start,
                       FilterOp op,
                       BitVector::Builder& builder) {
  switch (op) {
    case FilterOp::kEq:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::equal_to<T>(), builder);
    case FilterOp::kNe:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::not_equal_to<T>(), builder);
    case FilterOp::kLe:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::less_equal<T>(), builder);
    case FilterOp::kLt:
      return utils::LinearSearchWithComparator(typed_val, start, std::less<T>(),
                                               builder);
    case FilterOp::kGt:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::greater<T>(), builder);
    case FilterOp::kGe:
      return utils::LinearSearchWithComparator(
          typed_val, start, std::greater_equal<T>(), builder);
    case FilterOp::kGlob:
    case FilterOp::kRegex:
    case FilterOp::kIsNotNull:
    case FilterOp::kIsNull:
      PERFETTO_DFATAL("Illegal argument");
  }
}

}  // namespace

RangeOrBitVector NumericStorage::Search(FilterOp op,
                                        SqlValue value,
                                        RowMap::Range range) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NumericStorage::LinearSearch",
                    [&range, op](metatrace::Record* r) {
                      r->AddArg("Start", std::to_string(range.start));
                      r->AddArg("End", std::to_string(range.end));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });
  if (is_sorted_)
    return RangeOrBitVector(BinarySearchIntrinsic(op, value, range));
  return RangeOrBitVector(LinearSearchInternal(op, value, range));
}

RangeOrBitVector NumericStorage::IndexSearch(FilterOp op,
                                             SqlValue value,
                                             uint32_t* indices,
                                             uint32_t indices_count,
                                             bool sorted) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NumericStorage::IndexSearch",
                    [indices_count, op](metatrace::Record* r) {
                      r->AddArg("Count", std::to_string(indices_count));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });
  if (sorted) {
    return RangeOrBitVector(
        BinarySearchExtrinsic(op, value, indices, indices_count));
  }
  return RangeOrBitVector(
      IndexSearchInternal(op, value, indices, indices_count));
}

BitVector NumericStorage::LinearSearchInternal(FilterOp op,
                                               SqlValue sql_val,
                                               RowMap::Range range) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (op == FilterOp::kIsNotNull)
    return BitVector(size(), true);

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return BitVector(size(), false);

  BitVector::Builder builder(range.end, range.start);
  if (const auto* u32 = std::get_if<uint32_t>(&*val)) {
    auto* start = static_cast<const uint32_t*>(data_) + range.start;
    TypedLinearSearch(*u32, start, op, builder);
  } else if (const auto* i64 = std::get_if<int64_t>(&*val)) {
    auto* start = static_cast<const int64_t*>(data_) + range.start;
    TypedLinearSearch(*i64, start, op, builder);
  } else if (const auto* i32 = std::get_if<int32_t>(&*val)) {
    auto* start = static_cast<const int32_t*>(data_) + range.start;
    TypedLinearSearch(*i32, start, op, builder);
  } else if (const auto* db = std::get_if<double>(&*val)) {
    auto* start = static_cast<const double*>(data_) + range.start;
    TypedLinearSearch(*db, start, op, builder);
  } else {
    PERFETTO_DFATAL("Invalid");
  }
  return std::move(builder).Build();
}

BitVector NumericStorage::IndexSearchInternal(FilterOp op,
                                              SqlValue sql_val,
                                              uint32_t* indices,
                                              uint32_t indices_count) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (op == FilterOp::kIsNotNull)
    return BitVector(indices_count, true);

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return BitVector(indices_count, false);

  BitVector::Builder builder(indices_count);
  std::visit(
      [this, indices, op, &builder](auto val) {
        using T = decltype(val);
        auto* start = static_cast<const T*>(data_);
        std::visit(
            [start, indices, val, &builder](auto comparator) {
              utils::IndexSearchWithComparator(val, start, indices, comparator,
                                               builder);
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
    return search_range;

  if (!val.has_value() || op == FilterOp::kIsNull || op == FilterOp::kGlob)
    return RowMap::Range();

  switch (op) {
    case FilterOp::kEq:
      return RowMap::Range(LowerBoundIntrinsic(data_, *val, search_range),
                           UpperBoundIntrinsic(data_, *val, search_range));
    case FilterOp::kLe: {
      return RowMap::Range(search_range.start,
                           UpperBoundIntrinsic(data_, *val, search_range));
    }
    case FilterOp::kLt:
      return RowMap::Range(search_range.start,
                           LowerBoundIntrinsic(data_, *val, search_range));
    case FilterOp::kGe:
      return RowMap::Range(LowerBoundIntrinsic(data_, *val, search_range),
                           search_range.end);
    case FilterOp::kGt:
      return RowMap::Range(UpperBoundIntrinsic(data_, *val, search_range),
                           search_range.end);
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
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
    case FilterOp::kRegex:
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
