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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

// Converting a value a column is filtered by to the column's type: the rules
// SQL comparisons follow, shared by everything which filters columns. A value
// comes from a fetcher (see ValueFetcher), at an index whose meaning is the
// fetcher's.
namespace perfetto::trace_processor::core::filter {

// Result of casting a filter value for comparison during query execution.
struct CastFilterValueResult {
  enum Validity : uint8_t { kValid, kAllMatch, kNoneMatch };

  // Cast value for Id columns.
  struct Id {
    bool operator==(const Id& other) const { return value == other.value; }
    bool operator<(const Id& other) const { return value < other.value; }
    template <typename H>
    friend H PerfettoHashValue(H h, const Id& id) {
      return H::Combine(std::move(h), id.value);
    }
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

// Result of casting an IN clause's value list.
//
// The canonical storage is a typed HashMap (ValueHashMap) which naturally
// deduplicates values at cast time. A typed sorted ValueList is derived
// from it for the linear scan and indexed binary search paths. For dense
// Id/Uint32 values, a BitVector provides O(1) membership testing.
struct CastFilterValueListResult {
  using Value = std::variant<CastFilterValueResult::Id,
                             uint32_t,
                             int32_t,
                             int64_t,
                             double,
                             StringPool::Id>;
  template <typename K>
  using HashMap = base::FlatHashMapV2<K, bool>;
  using ValueHashMap = std::variant<HashMap<CastFilterValueResult::Id>,
                                    HashMap<uint32_t>,
                                    HashMap<int32_t>,
                                    HashMap<int64_t>,
                                    HashMap<double>,
                                    HashMap<StringPool::Id>>;
  using ValueList = std::variant<FlexVector<CastFilterValueResult::Id>,
                                 FlexVector<uint32_t>,
                                 FlexVector<int32_t>,
                                 FlexVector<int64_t>,
                                 FlexVector<double>,
                                 FlexVector<StringPool::Id>>;

  using Ptr = std::unique_ptr<CastFilterValueListResult>;

  // Initializes the hash_map and value_list variants to the correct
  // alternative for storage type T. Must be called before accessing
  // these fields via unchecked_get.
  template <typename T>
  void Init() {
    hash_map.emplace<StorageType::VariantTypeAtIndex<T, ValueHashMap>>();
    value_list.emplace<StorageType::VariantTypeAtIndex<T, ValueList>>();
  }

  // Resets all fields to their default state while preserving heap
  // allocations inside the HashMap, ValueList, and BitVector.
  template <typename T>
  void Clear() {
    validity = CastFilterValueResult::Validity::kNoneMatch;
    base::unchecked_get<StorageType::VariantTypeAtIndex<T, ValueHashMap>>(
        hash_map)
        .Clear();
    base::unchecked_get<StorageType::VariantTypeAtIndex<T, ValueList>>(
        value_list)
        .clear();
    bit_vector.clear();
  }

  CastFilterValueResult::Validity validity =
      CastFilterValueResult::Validity::kNoneMatch;

  // Typed HashMap for O(1) membership testing and deduplication.
  ValueHashMap hash_map;

  // For dense Id/Uint32 values, a BitVector for O(1) membership testing.
  // Empty when not applicable.
  BitVector bit_vector;

  // Deduplicated typed values for linear scan (small lists) and indexed
  // binary search paths. Empty for large lists.
  ValueList value_list;
};

// Handles conversion of strings or nulls to integer or double types for
// filtering operations.
template <typename FilterValueFetcherImpl>
inline PERFETTO_ALWAYS_INLINE CastFilterValueResult::Validity
CastStringOrNullFilterValueToIntegerOrDouble(
    typename FilterValueFetcherImpl::Type filter_value_type,
    NonStringOp op) {
  if (filter_value_type == FilterValueFetcherImpl::kString) {
    if (op.index() == NonStringOp::GetTypeIndex<Eq>() ||
        op.index() == NonStringOp::GetTypeIndex<Ge>() ||
        op.index() == NonStringOp::GetTypeIndex<Gt>()) {
      return CastFilterValueResult::kNoneMatch;
    }
    PERFETTO_DCHECK(op.index() == NonStringOp::GetTypeIndex<Ne>() ||
                    op.index() == NonStringOp::GetTypeIndex<Le>() ||
                    op.index() == NonStringOp::GetTypeIndex<Lt>());
    return CastFilterValueResult::kAllMatch;
  }

  PERFETTO_DCHECK(filter_value_type == FilterValueFetcherImpl::kNull);

  // Nulls always compare false to any value (including other nulls),
  // regardless of the operator.
  return CastFilterValueResult::kNoneMatch;
}

// Converts a double to an integer type using the specified function
// (e.g., trunc, floor). Used as a helper for various casting operations.
template <typename T, double (*fn)(double)>
inline PERFETTO_ALWAYS_INLINE CastFilterValueResult::Validity
CastDoubleToIntHelper(bool no_data, bool all_data, double d, T& out) {
  if (no_data) {
    return CastFilterValueResult::kNoneMatch;
  }
  if (all_data) {
    return CastFilterValueResult::kAllMatch;
  }
  out = static_cast<T>(fn(d));
  return CastFilterValueResult::kValid;
}

// Attempts to cast a filter value to an integer type, handling various
// edge cases such as out-of-range values and non-integer inputs.
template <typename T, typename FilterValueFetcherImpl>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE CastFilterValueResult::Validity
CastFilterValueToInteger(
    uint32_t index,
    typename FilterValueFetcherImpl::Type filter_value_type,
    FilterValueFetcherImpl& fetcher,
    NonStringOp op,
    T& out) {
  static_assert(std::is_integral_v<T>, "Unsupported type");

  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kInt64)) {
    int64_t res = fetcher.GetInt64Value(index);
    bool is_small = res < std::numeric_limits<T>::min();
    bool is_big = res > std::numeric_limits<T>::max();
    if (PERFETTO_UNLIKELY(is_small || is_big)) {
      switch (op.index()) {
        case NonStringOp::GetTypeIndex<Lt>():
        case NonStringOp::GetTypeIndex<Le>():
          if (is_small) {
            return CastFilterValueResult::kNoneMatch;
          }
          break;
        case NonStringOp::GetTypeIndex<Gt>():
        case NonStringOp::GetTypeIndex<Ge>():
          if (is_big) {
            return CastFilterValueResult::kNoneMatch;
          }
          break;
        case NonStringOp::GetTypeIndex<Eq>():
          return CastFilterValueResult::kNoneMatch;
        case NonStringOp::GetTypeIndex<Ne>():
          // Do nothing.
          break;
        default:
          PERFETTO_FATAL("Invalid numeric filter op");
      }
      return CastFilterValueResult::kAllMatch;
    }
    out = static_cast<T>(res);
    return CastFilterValueResult::kValid;
  }
  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kDouble)) {
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
      return CastFilterValueResult::kNoneMatch;
    }

    // The greater than or equal is intentional to account for the fact
    // that twos-complement integers are not symmetric around zero (i.e.
    // -9223372036854775808 can be represented but 9223372036854775808
    // cannot).
    bool is_big = d >= kMax;
    bool is_small = d < kMin;
    if (PERFETTO_LIKELY(d == trunc(d) && !is_small && !is_big)) {
      out = static_cast<T>(d);
      return CastFilterValueResult::kValid;
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
        return CastFilterValueResult::kNoneMatch;
      case NonStringOp::GetTypeIndex<Ne>():
        // Do nothing.
        return CastFilterValueResult::kAllMatch;
      default:
        PERFETTO_FATAL("Invalid numeric filter op");
    }
  }
  return CastStringOrNullFilterValueToIntegerOrDouble<FilterValueFetcherImpl>(
      filter_value_type, op);
}

// Attempts to cast a filter value to a double, handling integer inputs
// and various edge cases.
template <typename FilterValueFetcherImpl>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE CastFilterValueResult::Validity
CastFilterValueToDouble(uint32_t index,
                        typename FilterValueFetcherImpl::Type filter_value_type,
                        FilterValueFetcherImpl& fetcher,
                        NonStringOp op,
                        double& out) {
  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kDouble)) {
    out = fetcher.GetDoubleValue(index);
    return CastFilterValueResult::kValid;
  }
  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kInt64)) {
    int64_t i = fetcher.GetInt64Value(index);
    auto iad = static_cast<double>(i);
    auto iad_int = static_cast<int64_t>(iad);

    // If the integer value can be converted to a double while preserving
    // the exact integer value, then we can use the double value for
    // comparison.
    if (PERFETTO_LIKELY(i == iad_int)) {
      out = iad;
      return CastFilterValueResult::kValid;
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
        return CastFilterValueResult::kValid;
      case NonStringOp::GetTypeIndex<Le>():
        out =
            iad_less_than_i
                ? iad
                : std::nextafter(iad, -std::numeric_limits<double>::infinity());
        return CastFilterValueResult::kValid;
      case NonStringOp::GetTypeIndex<Gt>():
        out =
            iad_less_than_i
                ? iad
                : std::nextafter(iad, -std::numeric_limits<double>::infinity());
        return CastFilterValueResult::kValid;
      case NonStringOp::GetTypeIndex<Ge>():
        out =
            iad_greater_than_i
                ? iad
                : std::nextafter(iad, std::numeric_limits<double>::infinity());
        return CastFilterValueResult::kValid;
      case NonStringOp::GetTypeIndex<Eq>():
        return CastFilterValueResult::kNoneMatch;
      case NonStringOp::GetTypeIndex<Ne>():
        // Do nothing.
        return CastFilterValueResult::kAllMatch;
      default:
        PERFETTO_FATAL("Invalid numeric filter op");
    }
  }
  return CastStringOrNullFilterValueToIntegerOrDouble<FilterValueFetcherImpl>(
      filter_value_type, op);
}

// Attempts to cast a filter value to a numeric type, dispatching to the
// appropriate type-specific conversion function.
template <typename T, typename FilterValueFetcherImpl>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE CastFilterValueResult::Validity
CastFilterValueToIntegerOrDouble(
    uint32_t index,
    typename FilterValueFetcherImpl::Type filter_value_type,
    FilterValueFetcherImpl& fetcher,
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

template <typename FilterValueFetcherImpl>
inline PERFETTO_ALWAYS_INLINE CastFilterValueResult::Validity
CastFilterValueToString(uint32_t index,
                        typename FilterValueFetcherImpl::Type filter_value_type,
                        FilterValueFetcherImpl& fetcher,
                        const StringOp& op,
                        const char*& out) {
  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kString)) {
    out = fetcher.GetStringValue(index);
    return CastFilterValueResult::kValid;
  }
  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kNull)) {
    // Nulls always compare false to any value (including other nulls),
    // regardless of the operator.
    return CastFilterValueResult::kNoneMatch;
  }
  if (PERFETTO_LIKELY(filter_value_type == FilterValueFetcherImpl::kInt64 ||
                      filter_value_type == FilterValueFetcherImpl::kDouble)) {
    switch (op.index()) {
      case Op::GetTypeIndex<Ge>():
      case Op::GetTypeIndex<Gt>():
      case Op::GetTypeIndex<Ne>():
        return CastFilterValueResult::kAllMatch;
      case Op::GetTypeIndex<Eq>():
      case Op::GetTypeIndex<Le>():
      case Op::GetTypeIndex<Lt>():
      case Op::GetTypeIndex<Glob>():
      case Op::GetTypeIndex<Regex>():
        return CastFilterValueResult::kNoneMatch;
      default:
        PERFETTO_FATAL("Invalid string filter op");
    }
  }
  PERFETTO_FATAL("Invalid filter spec value");
}

// Below this threshold, the non-indexed FilterIn path does a linear scan
// over the value list instead of a HashMap lookup (avoids hashing overhead).
inline constexpr uint32_t kFilterInKeyScanThreshold = 16;

// Above this threshold, the indexed FilterIn path switches from binary
// search to copying the index and filtering in-place with the HashMap.
//
// Benchmarks (BM_FilterIn_IndexedBinarySearch / BM_FilterIn_IndexedLinearScan,
// run with BENCHMARK_CONSTANT_SWEEPING=1) show the crossover is consistently
// between k=50 and k=200 regardless of n, because the O(m·log(m)) sort of
// matched results dominates the binary search path when k is large. A threshold
// of 64 catches the crossover conservatively.
inline constexpr uint32_t kFilterInIndexedBinarySearchThreshold = 64;

// value_list is needed by both the key-scan and binary-search paths.
inline constexpr uint32_t kFilterInValueListThreshold =
    std::max(kFilterInKeyScanThreshold, kFilterInIndexedBinarySearchThreshold);

// Extracts the raw uint32_t from an Id or Uint32 key.
template <typename T>
uint32_t FilterInKeyToUint32(const T& key) {
  if constexpr (std::is_same_v<T, CastFilterValueResult::Id>) {
    return key.value;
  } else {
    static_assert(std::is_same_v<T, uint32_t>);
    return key;
  }
}

// If values are dense Id/Uint32, builds a BitVector from the HashMap.
template <typename T>
void MaybeBuildBitVector(CastFilterValueListResult& result) {
  if constexpr (std::is_same_v<T, Id> || std::is_same_v<T, Uint32>) {
    using HM = StorageType::VariantTypeAtIndex<
        T, CastFilterValueListResult::ValueHashMap>;
    auto& hm = base::unchecked_get<HM>(result.hash_map);
    uint32_t max_val = 0;
    for (auto it = hm.GetIterator(); it; ++it) {
      max_val = std::max(max_val, FilterInKeyToUint32(it.key()));
    }
    if (max_val <= static_cast<uint32_t>(hm.size()) * 16) {
      result.bit_vector.resize(max_val + 1);
      result.bit_vector.ClearAllBits();
      for (auto it = hm.GetIterator(); it; ++it) {
        result.bit_vector.set(FilterInKeyToUint32(it.key()));
      }
    }
  }
}

// Builds a value_list from the HashMap when the key count is small enough
// for the linear scan or indexed binary search paths.
template <typename T>
void MaybeBuildValueList(CastFilterValueListResult& result) {
  using HM =
      StorageType::VariantTypeAtIndex<T,
                                      CastFilterValueListResult::ValueHashMap>;
  using VL =
      StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::ValueList>;
  auto& hm = base::unchecked_get<HM>(result.hash_map);
  if (hm.size() > kFilterInValueListThreshold) {
    return;
  }
  auto& vl = base::unchecked_get<VL>(result.value_list);
  for (auto it = hm.GetIterator(); it; ++it) {
    vl.push_back(it.key());
  }
}

// Casts the values of an IN list for a column of storage type T, as `op`
// (always Eq) casts each: the values are those the fetcher iterates at
// `index`. A string is looked up in `string_pool`, and matches nothing if it
// is not there. `result` must have been initialized or cleared for T.
template <typename T, typename FilterValueFetcherImpl>
inline PERFETTO_ALWAYS_INLINE void CastFilterValueList(
    uint32_t index,
    FilterValueFetcherImpl& fetcher,
    const NonNullOp& cast_op,
    const StringPool* string_pool,
    CastFilterValueListResult& result) {
  using ValueType =
      StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::Value>;
  using HM =
      StorageType::VariantTypeAtIndex<T,
                                      CastFilterValueListResult::ValueHashMap>;
  auto& hm = base::unchecked_get<HM>(result.hash_map);
  bool all_match = false;
  for (bool has_more = fetcher.IteratorInit(index); has_more;
       has_more = fetcher.IteratorNext(index)) {
    typename FilterValueFetcherImpl::Type filter_value_type =
        fetcher.GetValueType(index);
    if constexpr (std::is_same_v<T, Id>) {
      auto op = *cast_op.TryDowncast<NonStringOp>();
      uint32_t result_value;
      auto validity =
          CastFilterValueToInteger<uint32_t, FilterValueFetcherImpl>(
              index, filter_value_type, fetcher, op, result_value);
      if (PERFETTO_LIKELY(validity == CastFilterValueResult::kValid)) {
        hm.Insert(CastFilterValueResult::Id{result_value}, true);
      } else if (validity == CastFilterValueResult::kAllMatch) {
        all_match = true;
        break;
      }
    } else if constexpr (IntegerOrDoubleType::Contains<T>()) {
      auto op = *cast_op.TryDowncast<NonStringOp>();
      ValueType result_value;
      auto validity =
          CastFilterValueToIntegerOrDouble<ValueType, FilterValueFetcherImpl>(
              index, filter_value_type, fetcher, op, result_value);
      if (PERFETTO_LIKELY(validity == CastFilterValueResult::kValid)) {
        hm.Insert(result_value, true);
      } else if (validity == CastFilterValueResult::kAllMatch) {
        all_match = true;
        break;
      }
    } else if constexpr (std::is_same_v<T, String>) {
      auto op = *cast_op.TryDowncast<StringOp>();
      PERFETTO_CHECK(op.Is<Eq>());
      const char* result_value;
      auto validity = CastFilterValueToString<FilterValueFetcherImpl>(
          index, filter_value_type, fetcher, op, result_value);
      if (PERFETTO_LIKELY(validity == CastFilterValueResult::kValid)) {
        auto id = string_pool->GetId(result_value);
        if (id) {
          hm.Insert(*id, true);
        }
      } else if (validity == CastFilterValueResult::kAllMatch) {
        all_match = true;
        break;
      }
    } else {
      static_assert(std::is_same_v<T, Id>, "Unsupported type");
    }
  }
  if (all_match) {
    result.validity = CastFilterValueResult::Validity::kAllMatch;
  } else if (hm.size() == 0) {
    result.validity = CastFilterValueResult::Validity::kNoneMatch;
  } else {
    result.validity = CastFilterValueResult::Validity::kValid;
    MaybeBuildBitVector<T>(result);
    MaybeBuildValueList<T>(result);
  }
}

}  // namespace perfetto::trace_processor::core::filter

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_VALUE_CAST_H_
