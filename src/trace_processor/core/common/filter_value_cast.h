/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_VALUE_CAST_H_
#define SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_VALUE_CAST_H_

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"

namespace perfetto::trace_processor::core::ops {

// What narrowing a caller's value to a column's type produced.
//
// Narrowing is where a comparison can stop being about a value: asking for
// 1e300 as an int32 excludes every row, asking for -1 as an unsigned excludes
// none of them. Both backends have to agree on that, so the rules live here
// and neither restates them.
enum class CastResult : uint8_t {
  kValid,
  kAllMatch,
  kNoneMatch,
};

// Handles conversion of strings or nulls to integer or double types for
// filtering operations.
inline PERFETTO_ALWAYS_INLINE CastResult
CastStringOrNullFilterValueToIntegerOrDouble(
    ValueFetcher::Type filter_value_type,
    NonStringOp op) {
  if (filter_value_type == ValueFetcher::Type::kString) {
    if (op.index() == NonStringOp::GetTypeIndex<Eq>() ||
        op.index() == NonStringOp::GetTypeIndex<Ge>() ||
        op.index() == NonStringOp::GetTypeIndex<Gt>()) {
      return CastResult::kNoneMatch;
    }
    PERFETTO_DCHECK(op.index() == NonStringOp::GetTypeIndex<Ne>() ||
                    op.index() == NonStringOp::GetTypeIndex<Le>() ||
                    op.index() == NonStringOp::GetTypeIndex<Lt>());
    return CastResult::kAllMatch;
  }

  PERFETTO_DCHECK(filter_value_type == ValueFetcher::Type::kNull);

  // Nulls always compare false to any value (including other nulls),
  // regardless of the operator.
  return CastResult::kNoneMatch;
}

// Converts a double to an integer type using the specified function
// (e.g., trunc, floor). Used as a helper for various casting operations.
template <typename T, double (*fn)(double)>
inline PERFETTO_ALWAYS_INLINE CastResult
CastDoubleToIntHelper(bool no_data, bool all_data, double d, T& out) {
  if (no_data) {
    return CastResult::kNoneMatch;
  }
  if (all_data) {
    return CastResult::kAllMatch;
  }
  out = static_cast<T>(fn(d));
  return CastResult::kValid;
}

// Attempts to cast a filter value to an integer type, handling various
// edge cases such as out-of-range values and non-integer inputs.
template <typename T>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE CastResult
CastFilterValueToInteger(uint32_t index,
                         ValueFetcher::Type filter_value_type,
                         ValueFetcher& fetcher,
                         NonStringOp op,
                         T& out) {
  static_assert(std::is_integral_v<T>, "Unsupported type");

  if (PERFETTO_LIKELY(filter_value_type == ValueFetcher::Type::kInt64)) {
    int64_t res = fetcher.GetInt64Value(index);
    bool is_small = res < std::numeric_limits<T>::min();
    bool is_big = res > std::numeric_limits<T>::max();
    if (PERFETTO_UNLIKELY(is_small || is_big)) {
      switch (op.index()) {
        case NonStringOp::GetTypeIndex<Lt>():
        case NonStringOp::GetTypeIndex<Le>():
          if (is_small) {
            return CastResult::kNoneMatch;
          }
          break;
        case NonStringOp::GetTypeIndex<Gt>():
        case NonStringOp::GetTypeIndex<Ge>():
          if (is_big) {
            return CastResult::kNoneMatch;
          }
          break;
        case NonStringOp::GetTypeIndex<Eq>():
          return CastResult::kNoneMatch;
        case NonStringOp::GetTypeIndex<Ne>():
          // Do nothing.
          break;
        default:
          PERFETTO_FATAL("Invalid numeric filter op");
      }
      return CastResult::kAllMatch;
    }
    out = static_cast<T>(res);
    return CastResult::kValid;
  }
  if (PERFETTO_LIKELY(filter_value_type == ValueFetcher::Type::kDouble)) {
    double d = fetcher.GetDoubleValue(index);

    // We use the constants directly instead of using numeric_limits for
    // int64_t as the casts introduces rounding in the doubles as a double
    // cannot exactly represent int64::max().
    constexpr double kMin =
        std::is_same_v<T, int64_t>
            ? -9223372036854775808.0
            : static_cast<double>(std::numeric_limits<T>::min());
    constexpr double kMax =
        std::is_same_v<T, int64_t>
            ? 9223372036854775808.0
            : static_cast<double>(std::numeric_limits<T>::max());

    // NaNs always compare false to any value (including other NaNs),
    // regardless of the operator.
    if (PERFETTO_UNLIKELY(std::isnan(d))) {
      return CastResult::kNoneMatch;
    }

    // The greater than or equal is intentional to account for the fact
    // that twos-complement integers are not symmetric around zero (i.e.
    // -9223372036854775808 can be represented but 9223372036854775808
    // cannot).
    bool is_big = d >= kMax;
    bool is_small = d < kMin;
    if (PERFETTO_LIKELY(d == trunc(d) && !is_small && !is_big)) {
      out = static_cast<T>(d);
      return CastResult::kValid;
    }
    switch (op.index()) {
      case NonStringOp::GetTypeIndex<Lt>():
        return CastDoubleToIntHelper<T, std::ceil>(is_small, is_big, d, out);
      case NonStringOp::GetTypeIndex<Le>():
        return CastDoubleToIntHelper<T, std::floor>(is_small, is_big, d, out);
      case NonStringOp::GetTypeIndex<Gt>():
        return CastDoubleToIntHelper<T, std::floor>(is_big, is_small, d, out);
      case NonStringOp::GetTypeIndex<Ge>():
        return CastDoubleToIntHelper<T, std::ceil>(is_big, is_small, d, out);
      case NonStringOp::GetTypeIndex<Eq>():
        return CastResult::kNoneMatch;
      case NonStringOp::GetTypeIndex<Ne>():
        // Do nothing.
        return CastResult::kAllMatch;
      default:
        PERFETTO_FATAL("Invalid numeric filter op");
    }
  }
  return CastStringOrNullFilterValueToIntegerOrDouble(filter_value_type, op);
}

// Attempts to cast a filter value to a double, handling integer inputs
// and various edge cases.
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE CastResult
CastFilterValueToDouble(uint32_t index,
                        ValueFetcher::Type filter_value_type,
                        ValueFetcher& fetcher,
                        NonStringOp op,
                        double& out) {
  if (PERFETTO_LIKELY(filter_value_type == ValueFetcher::Type::kDouble)) {
    out = fetcher.GetDoubleValue(index);
    return CastResult::kValid;
  }
  if (PERFETTO_LIKELY(filter_value_type == ValueFetcher::Type::kInt64)) {
    int64_t i = fetcher.GetInt64Value(index);
    auto iad = static_cast<double>(i);
    auto iad_int = static_cast<int64_t>(iad);

    // If the integer value can be converted to a double while preserving
    // the exact integer value, then we can use the double value for
    // comparison.
    if (PERFETTO_LIKELY(i == iad_int)) {
      out = iad;
      return CastResult::kValid;
    }

    // This can happen in cases where we round `i` up above
    // numeric_limits::max(). In that case, still consider the double
    // larger.
    bool overflow_positive_to_negative = i > 0 && iad_int < 0;
    bool iad_greater_than_i = iad_int > i || overflow_positive_to_negative;
    bool iad_less_than_i = iad_int < i && !overflow_positive_to_negative;
    switch (op.index()) {
      case NonStringOp::GetTypeIndex<Lt>():
        out =
            iad_greater_than_i
                ? iad
                : std::nextafter(iad, std::numeric_limits<double>::infinity());
        return CastResult::kValid;
      case NonStringOp::GetTypeIndex<Le>():
        out =
            iad_less_than_i
                ? iad
                : std::nextafter(iad, -std::numeric_limits<double>::infinity());
        return CastResult::kValid;
      case NonStringOp::GetTypeIndex<Gt>():
        out =
            iad_less_than_i
                ? iad
                : std::nextafter(iad, -std::numeric_limits<double>::infinity());
        return CastResult::kValid;
      case NonStringOp::GetTypeIndex<Ge>():
        out =
            iad_greater_than_i
                ? iad
                : std::nextafter(iad, std::numeric_limits<double>::infinity());
        return CastResult::kValid;
      case NonStringOp::GetTypeIndex<Eq>():
        return CastResult::kNoneMatch;
      case NonStringOp::GetTypeIndex<Ne>():
        // Do nothing.
        return CastResult::kAllMatch;
      default:
        PERFETTO_FATAL("Invalid numeric filter op");
    }
  }
  return CastStringOrNullFilterValueToIntegerOrDouble(filter_value_type, op);
}

// Attempts to cast a filter value to a numeric type, dispatching to the
// appropriate type-specific conversion function.
template <typename T>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE CastResult
CastFilterValueToIntegerOrDouble(uint32_t index,
                                 ValueFetcher::Type filter_value_type,
                                 ValueFetcher& fetcher,
                                 NonStringOp op,
                                 T& out) {
  if constexpr (std::is_same_v<T, double>) {
    return CastFilterValueToDouble(index, filter_value_type, fetcher, op, out);
  } else if constexpr (std::is_integral_v<T>) {
    return CastFilterValueToInteger<T>(index, filter_value_type, fetcher, op,
                                       out);
  } else {
    static_assert(std::is_same_v<T, double>, "Unsupported type");
  }
}

}  // namespace perfetto::trace_processor::core::ops

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_VALUE_CAST_H_
