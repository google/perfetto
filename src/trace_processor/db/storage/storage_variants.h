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
#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_VARIANTS_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_VARIANTS_H_

#include <variant>

#include "src/trace_processor/db/storage/types.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

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

// Based on SqlValue and ColumnType, casts SqlValue to proper type, returns
// std::nullopt if SqlValue can't be cast and should be considered invalid for
// comparison.
inline std::optional<NumericValue> GetNumericTypeVariant(ColumnType type) {
  return GetNumericTypeVariant(type, SqlValue::Long(0));
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

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_VARIANTS_H_
