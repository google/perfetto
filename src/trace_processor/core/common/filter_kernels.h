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

#ifndef SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_KERNELS_H_
#define SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_KERNELS_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

// Keeping the rows of a column which compare true with a value already cast
// to the column's type (see filter_value_cast.h). Shared by everything which
// filters columns.
namespace perfetto::trace_processor::core::filter {

namespace comparators {

// Returns an appropriate comparator functor for the given integer/double type
// and operation. Currently only supports equality comparison.
template <typename T, typename Op>
auto IntegerOrDoubleComparator() {
  if constexpr (std::is_same_v<Op, Eq>) {
    return std::equal_to<T>();
  } else if constexpr (std::is_same_v<Op, Ne>) {
    return std::not_equal_to<T>();
  } else if constexpr (std::is_same_v<Op, Lt>) {
    return std::less<T>();
  } else if constexpr (std::is_same_v<Op, Le>) {
    return std::less_equal<T>();
  } else if constexpr (std::is_same_v<Op, Gt>) {
    return std::greater<T>();
  } else if constexpr (std::is_same_v<Op, Ge>) {
    return std::greater_equal<T>();
  } else {
    static_assert(std::is_same_v<Op, Eq>, "Unsupported op");
  }
}

template <typename T>
struct StringComparator {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    if constexpr (std::is_same_v<T, Lt>) {
      return pool_->Get(lhs) < rhs;
    } else if constexpr (std::is_same_v<T, Le>) {
      return pool_->Get(lhs) <= rhs;
    } else if constexpr (std::is_same_v<T, Gt>) {
      return pool_->Get(lhs) > rhs;
    } else if constexpr (std::is_same_v<T, Ge>) {
      return pool_->Get(lhs) >= rhs;
    } else {
      static_assert(std::is_same_v<T, Lt>, "Unsupported op");
    }
  }
  const StringPool* pool_;
};
struct StringLessInvert {
  bool operator()(NullTermStringView lhs, StringPool::Id rhs) const {
    return lhs < pool_->Get(rhs);
  }
  const StringPool* pool_;
};

}  // namespace comparators

// Filters an existing index buffer in-place, based on data comparisons
// performed using a separate set of source indices.
//
// This function iterates synchronously through two sets of indices:
// 1. Source Indices: Provided by [begin, end), pointed to by `it`. These
//    indices are used *only* to look up data values (`data[*it]`).
// 2. Destination/Update Indices: Starting at `o_start`, pointed to by
//    `o_read` (for reading the original index) and `o_write` (for writing
//    kept indices). This buffer is modified *in-place*.
//
// For each step `i`:
//   - It retrieves the data value using the i-th source index:
//   `data[begin[i]]`.
//   - It compares this data value against the provided `value`.
//   - It reads the i-th *original* index from the destination buffer:
//   `o_read[i]`.
//   - If the comparison is true, it copies the original index `o_read[i]`
//     to the current write position `*o_write` and advances `o_write`.
//
// The result is that the destination buffer `[o_start, returned_pointer)`
// contains the subset of its *original* indices for which the comparison
// (using the corresponding source index for data lookup) was true.
//
// Use Case Example (SparseNull Filter):
//   - `[begin, end)` holds translated storage indices (for correct data
//     lookup).
//   - `o_start` points to the buffer holding original table indices (that
//     was have already been filtered by `NullFilter<IsNotNull>`).
//   - This function further filters the original table indices in `o_start`
//     based on data comparisons using the translated indices.
//
// Args:
//   data: Pointer to the start of the column's data storage.
//   begin: Pointer to the first index in the source span (for data lookup).
//   end: Pointer one past the last index in the source span.
//   o_start: Pointer to the destination/update buffer (filtered in-place).
//   value: The value to compare data against.
//   comparator: Functor implementing the comparison logic.
//
// Returns:
//   A pointer one past the last index written to the destination buffer.
template <typename Comparator, typename ValueType, typename DataType>
[[nodiscard]] PERFETTO_ALWAYS_INLINE uint32_t* Filter(
    const DataType* data,
    const uint32_t* begin,
    const uint32_t* end,
    uint32_t* output,
    const ValueType& value,
    const Comparator& comparator) {
  const uint32_t* o_read = output;
  uint32_t* o_write = output;
  for (const uint32_t* it = begin; it != end; ++it, ++o_read) {
    // The choice of a branchy implemntation is intentional: this seems faster
    // than trying to do something branchless, likely because the compiler is
    // helping us with branch prediction.
    if (comparator(data[*it], value)) {
      *o_write++ = *o_read;
    }
  }
  return o_write;
}

// Similar to Filter but operates directly on the identity values
// (indices) rather than dereferencing through a data array.
template <typename Comparator, typename ValueType>
[[nodiscard]] PERFETTO_ALWAYS_INLINE uint32_t* IdentityFilter(
    const uint32_t* begin,
    const uint32_t* end,
    uint32_t* output,
    const ValueType& value,
    Comparator comparator) {
  const uint32_t* o_read = output;
  uint32_t* o_write = output;
  for (const uint32_t* it = begin; it != end; ++it, ++o_read) {
    // The choice of a branchy implemntation is intentional: this seems faster
    // than trying to do something branchless, likely because the compiler is
    // helping us with branch prediction.
    if (comparator(*it, value)) {
      *o_write++ = *o_read;
    }
  }
  return o_write;
}

inline PERFETTO_ALWAYS_INLINE uint32_t* StringFilterEq(
    const StringPool* string_pool,
    const StringPool::Id* data,
    const uint32_t* begin,
    const uint32_t* end,
    uint32_t* output,
    const char* val) {
  std::optional<StringPool::Id> id = string_pool->GetId(base::StringView(val));
  if (!id) {
    return output;
  }
  static_assert(sizeof(StringPool::Id) == 4, "Id should be 4 bytes");
  return Filter(reinterpret_cast<const uint32_t*>(data), begin, end, output,
                id->raw_id(), std::equal_to<>());
}

inline PERFETTO_ALWAYS_INLINE uint32_t* StringFilterNe(
    const StringPool* string_pool,
    const StringPool::Id* data,
    const uint32_t* begin,
    const uint32_t* end,
    uint32_t* output,
    const char* val) {
  std::optional<StringPool::Id> id = string_pool->GetId(base::StringView(val));
  if (!id) {
    return output + (end - begin);
  }
  static_assert(sizeof(StringPool::Id) == 4, "Id should be 4 bytes");
  return Filter(reinterpret_cast<const uint32_t*>(data), begin, end, output,
                id->raw_id(), std::not_equal_to<>());
}

// Applies an equality or ordering comparison of strings: the operators every
// string comparison supports.
template <typename Op>
inline PERFETTO_ALWAYS_INLINE uint32_t* FilterStringCompare(
    const StringPool* string_pool,
    const StringPool::Id* data,
    const uint32_t* begin,
    const uint32_t* end,
    uint32_t* output,
    const char* val) {
  if constexpr (std::is_same_v<Op, Eq>) {
    return StringFilterEq(string_pool, data, begin, end, output, val);
  } else if constexpr (std::is_same_v<Op, Ne>) {
    return StringFilterNe(string_pool, data, begin, end, output, val);
  } else {
    return Filter(data, begin, end, output, NullTermStringView(val),
                  comparators::StringComparator<Op>{string_pool});
  }
}

// BitVector membership filter for FilterIn. O(1) per row.
// Only applicable to Id/Uint32 columns with dense value ranges.
template <typename T, typename DataType>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE uint32_t* FilterInBitVector(
    const DataType* data,
    const uint32_t* source_begin,
    const uint32_t* source_end,
    uint32_t* dest,
    const BitVector& bv) {
  struct Cmp {
    PERFETTO_ALWAYS_INLINE bool operator()(uint32_t lhs,
                                           const BitVector& b) const {
      return lhs < b.size() && b.is_set(lhs);
    }
  };
  if constexpr (std::is_same_v<T, Id>) {
    base::ignore_result(data);
    return IdentityFilter(source_begin, source_end, dest, bv, Cmp());
  } else {
    return Filter(data, source_begin, source_end, dest, bv, Cmp());
  }
}

// Linear scan membership filter for FilterIn. Iterates over a small sorted
// value list for each row. Avoids HashMap hashing overhead for small lists.
template <typename T, typename DataType, typename VL>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE uint32_t* FilterInLinearScan(
    const DataType* data,
    const uint32_t* source_begin,
    const uint32_t* source_end,
    uint32_t* dest,
    const VL& vl) {
  using ValElem =
      StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::Value>;
  if constexpr (std::is_same_v<T, Id>) {
    struct Cmp {
      PERFETTO_ALWAYS_INLINE bool operator()(
          uint32_t lhs,
          const FlexVector<ValElem>& k) const {
        for (const auto& v : k) {
          if (lhs == v.value)
            return true;
        }
        return false;
      }
    };
    return IdentityFilter(source_begin, source_end, dest, vl, Cmp());
  } else {
    using D = std::remove_cv_t<std::remove_reference_t<decltype(*data)>>;
    struct Cmp {
      PERFETTO_ALWAYS_INLINE bool operator()(
          D lhs,
          const FlexVector<ValElem>& k) const {
        for (const auto& v : k) {
          if (std::equal_to<>()(lhs, v))
            return true;
        }
        return false;
      }
    };
    return Filter(data, source_begin, source_end, dest, vl, Cmp());
  }
}

// HashMap membership filter for FilterIn. O(1) per row via hash lookup.
template <typename T, typename DataType, typename HM>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE uint32_t* FilterInHashMap(
    const DataType* data,
    const uint32_t* source_begin,
    const uint32_t* source_end,
    uint32_t* dest,
    const HM& hm) {
  if constexpr (std::is_same_v<T, Id>) {
    struct Cmp {
      PERFETTO_ALWAYS_INLINE bool operator()(uint32_t lhs, const HM& h) const {
        return h.Find(CastFilterValueResult::Id{lhs}) != nullptr;
      }
    };
    return IdentityFilter(source_begin, source_end, dest, hm, Cmp());
  } else {
    using D = std::remove_cv_t<std::remove_reference_t<decltype(*data)>>;
    struct Cmp {
      PERFETTO_ALWAYS_INLINE bool operator()(D lhs, const HM& h) const {
        return h.Find(lhs) != nullptr;
      }
    };
    return Filter(data, source_begin, source_end, dest, hm, Cmp());
  }
}

// Scan path for FilterIn when no index is present. The source span and dest
// span are walked in lockstep: source[i] is the storage index for dest[i].
// Matching dest entries are compacted in-place.
template <typename T, typename DataType>
inline PERFETTO_ALWAYS_INLINE void NonIndexedFilterInScan(
    const CastFilterValueListResult& cast_result,
    const DataType* data,
    const Span<uint32_t>& source,
    Span<uint32_t>& dest) {
  using HM =
      StorageType::VariantTypeAtIndex<T,
                                      CastFilterValueListResult::ValueHashMap>;
  using VL =
      StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::ValueList>;
  if constexpr (std::is_same_v<T, Id> || std::is_same_v<T, Uint32>) {
    if (cast_result.bit_vector.size() > 0) {
      dest.e = FilterInBitVector<T>(data, source.b, source.e, dest.b,
                                    cast_result.bit_vector);
      return;
    }
  }
  const auto& vl = base::unchecked_get<VL>(cast_result.value_list);
  if (!vl.empty() && vl.size() <= kFilterInKeyScanThreshold) {
    dest.e = FilterInLinearScan<T>(data, source.b, source.e, dest.b, vl);
    return;
  }
  const auto& hm = base::unchecked_get<HM>(cast_result.hash_map);
  dest.e = FilterInHashMap<T>(data, source.b, source.e, dest.b, hm);
}

}  // namespace perfetto::trace_processor::core::filter

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_KERNELS_H_
