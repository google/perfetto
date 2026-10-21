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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/util/galloping_search.h"

// Keeping the rows of a column which compare true with a value already cast
// to the column's type (see filter_value_cast.h), by testing each row or by
// searching a sorted column or index. Shared by everything which filters
// columns.
namespace perfetto::trace_processor::core::filter {

namespace comparators {

// Returns an appropriate comparator functor for the given integer/double type
// and operation.
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

// -----------------------------------------------------------------------------
// Searches of sorted columns and indexes
// -----------------------------------------------------------------------------
//
// A sorted column, or an index listing a column's rows in the order of their
// values, keeps the rows equal to or beyond a value together, so they are
// found by searching rather than testing every row. Each search narrows
// `range`, rows of the column or entries of the index, to those it keeps.

template <typename DataType>
inline auto SortedLowerBoundComparator(const StringPool* string_pool) {
  if constexpr (std::is_same_v<DataType, StringPool::Id>) {
    return comparators::StringComparator<Lt>{string_pool};
  } else {
    base::ignore_result(string_pool);
    return std::less<>();
  }
}

template <typename DataType>
inline auto SortedUpperBoundComparator(const StringPool* string_pool) {
  if constexpr (std::is_same_v<DataType, StringPool::Id>) {
    return comparators::StringLessInvert{string_pool};
  } else {
    base::ignore_result(string_pool);
    return std::less<>();
  }
}

// The first row of `range` whose value is not less than `val`, in a column
// sorted ascending.
template <typename DataType, typename ValueType>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE uint32_t
SortedLowerBound(const DataType* data,
                 ValueType val,
                 const StringPool* string_pool,
                 Range range) {
  return static_cast<uint32_t>(
      std::lower_bound(data + range.b, data + range.e, val,
                       SortedLowerBoundComparator<DataType>(string_pool)) -
      data);
}

// The first row of `range` whose value is greater than `val`, in a column
// sorted ascending.
template <typename DataType, typename ValueType>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE uint32_t
SortedUpperBound(const DataType* data,
                 ValueType val,
                 const StringPool* string_pool,
                 Range range) {
  return static_cast<uint32_t>(
      std::upper_bound(data + range.b, data + range.e, val,
                       SortedUpperBoundComparator<DataType>(string_pool)) -
      data);
}

// Narrows `range` to the rows whose value equals `val`, in a column sorted
// ascending.
template <typename DataType, typename ValueType>
inline PERFETTO_ALWAYS_INLINE void SortedEqualRange(
    const DataType* data,
    ValueType val,
    const StringPool* string_pool,
    Range& range) {
  auto* begin = data + range.b;
  auto* end = data + range.e;
  DataType cmp_value;
  if constexpr (std::is_same_v<DataType, StringPool::Id>) {
    std::optional<StringPool::Id> id =
        string_pool->GetId(base::StringView(val));
    if (!id) {
      range.e = range.b;
      return;
    }
    cmp_value = *id;
  } else {
    cmp_value = val;
  }
  const DataType* eq_start = std::lower_bound(
      begin, end, val, SortedLowerBoundComparator<DataType>(string_pool));
  // Equal rows are usually few, so search from the first of them outwards.
  const DataType* eq_end =
      GallopingPartitionPoint(eq_start, end, [cmp_value](const DataType& v) {
        return std::equal_to<>()(v, cmp_value);
      });
  range.b = static_cast<uint32_t>(eq_start - data);
  range.e = static_cast<uint32_t>(eq_end - data);
}

// Narrows `range` to the row of an Id column whose value is `val`: an Id's
// value is its row.
inline PERFETTO_ALWAYS_INLINE void IdEqualRange(uint32_t val, Range& range) {
  bool in_bounds = val >= range.b && val < range.e;
  range.b = val;
  range.e = val + in_bounds;
}

// Narrows `range` to the rows of an Id column from `val` on, if `begin`, or
// before it otherwise.
inline PERFETTO_ALWAYS_INLINE void IdBound(uint32_t val,
                                           bool begin,
                                           Range& range) {
  uint32_t new_b = begin ? std::max(range.b, val) : range.b;
  uint32_t new_e = !begin ? std::min(range.e, val) : range.e;
  range.b = new_b;
  range.e = std::max(new_b, new_e);
}

// Narrows `range` to the rows whose value equals `val`, in a Uint32 column
// sorted by SetId: the rows of a value start at the row of that number.
inline PERFETTO_ALWAYS_INLINE void
SetIdSortedEqualRange(const uint32_t* storage, uint32_t val, Range& range) {
  const auto* start =
      std::clamp(storage + val, storage + range.b, storage + range.e);
  range.b = static_cast<uint32_t>(start - storage);
  const uint32_t* it = GallopingPartitionPoint(
      start, storage + range.e, [val](uint32_t v) { return v == val; });
  range.e = static_cast<uint32_t>(it - storage);
}

// Narrows `range` to the row whose value is `val`, in a column whose values
// are distinct and small, stored as the set bits of `bv`: the row of a value
// is the number of values below it. `popcount` holds the count of set bits
// before each word of `bv`.
inline PERFETTO_ALWAYS_INLINE void SmallValueEqualRange(
    const BitVector& bv,
    const uint32_t* popcount,
    uint32_t val,
    Range& range) {
  uint32_t k = val < bv.size() && bv.is_set(val)
                   ? static_cast<uint32_t>(popcount[val / 64] +
                                           bv.count_set_bits_until_in_word(val))
                   : range.e;
  bool in_bounds = range.b <= k && k < range.e;
  range.b = in_bounds ? k : range.e;
  range.e = in_bounds ? k + 1 : range.b;
}

// Where the value of row `index` is stored in a column of nullability N, or
// UINT32_MAX if it is null. `bv` marks the rows with values; `popcount`, the
// count of set bits before each of its words, is read for SparseNull only.
template <typename N>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE uint32_t
IndexToStorageIndex(uint32_t index,
                    const BitVector* bv,
                    const uint32_t* popcount) {
  if constexpr (std::is_same_v<N, NonNull>) {
    base::ignore_result(bv, popcount);
    return index;
  } else if constexpr (std::is_same_v<N, SparseNull>) {
    if (!bv->is_set(index)) {
      // Null values are always less than non-null values.
      return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(popcount[index / 64] +
                                 bv->count_set_bits_until_in_word(index));
  } else if constexpr (std::is_same_v<N, DenseNull>) {
    base::ignore_result(popcount);
    return bv->is_set(index) ? index : std::numeric_limits<uint32_t>::max();
  } else {
    static_assert(std::is_same_v<N, NonNull>, "Unsupported type");
  }
}

// Binary-searches a sorted index for all entries whose resolved storage
// value equals |target|. Returns the [lb, ub) range of matching index
// entries.
//
// |target| should be a NullTermStringView for String columns (i.e. a
// pre-resolved string_pool->Get result) or the raw value for other types.
// This avoids repeated string_pool->Get calls inside the comparators.
template <typename T, typename N, typename Resolved>
inline PERFETTO_ALWAYS_INLINE std::pair<uint32_t*, uint32_t*> IndexEqualRange(
    uint32_t* begin,
    uint32_t* end,
    const Resolved& target,
    const typename T::cpp_type* data,
    const BitVector* bv,
    const uint32_t* popcount,
    const StringPool* string_pool) {
  auto* lb =
      std::lower_bound(begin, end, target, [&](uint32_t idx, const Resolved&) {
        uint32_t si = IndexToStorageIndex<N>(idx, bv, popcount);
        if (si == std::numeric_limits<uint32_t>::max())
          return true;
        if constexpr (std::is_same_v<T, String>) {
          return string_pool->Get(data[si]) < target;
        } else {
          return data[si] < target;
        }
      });
  auto* ub =
      std::upper_bound(lb, end, target, [&](const Resolved&, uint32_t idx) {
        uint32_t si = IndexToStorageIndex<N>(idx, bv, popcount);
        if (si == std::numeric_limits<uint32_t>::max())
          return false;
        if constexpr (std::is_same_v<T, String>) {
          return target < string_pool->Get(data[si]);
        } else {
          return target < data[si];
        }
      });
  return {lb, ub};
}

// The rows of an IN found by binary-searching a sorted index for each of its
// values, written to `dest` in increasing order. Returns false, writing
// nothing, if the list is empty or too long for searching to beat a scan.
//
// TODO(lalitm): since the index is sorted, galloping search could replace
// repeated std::lower_bound for better locality.
template <typename T, typename N>
[[nodiscard]] inline PERFETTO_ALWAYS_INLINE bool IndexedFilterInBinarySearch(
    const CastFilterValueListResult& cast_result,
    Span<uint32_t> index,
    const typename T::cpp_type* data,
    const BitVector* bv,
    const uint32_t* popcount,
    const StringPool* string_pool,
    Span<uint32_t>& dest) {
  using VL =
      StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::ValueList>;
  const VL& vl = base::unchecked_get<VL>(cast_result.value_list);
  if (vl.empty() || vl.size() > kFilterInIndexedBinarySearchThreshold) {
    return false;
  }
  uint32_t* write = dest.b;
  for (const auto& cmp_val : vl) {
    // For String columns, resolve StringPool::Id to NullTermStringView
    // once per value to avoid repeated lookups in the comparators.
    std::pair<uint32_t*, uint32_t*> range;
    if constexpr (std::is_same_v<T, String>) {
      auto target = string_pool->Get(cmp_val);
      range = IndexEqualRange<T, N>(index.b, index.e, target, data, bv,
                                    popcount, string_pool);
    } else {
      range = IndexEqualRange<T, N>(index.b, index.e, cmp_val, data, bv,
                                    popcount, string_pool);
    }
    auto n = static_cast<size_t>(range.second - range.first);
    memcpy(write, range.first, n * sizeof(uint32_t));
    write += n;
  }
  dest.e = write;

  // Binary search produces output in key order. Sort by raw index.
  std::sort(dest.b, dest.e);
  return true;
}

// The rows of `range` whose value is in an IN's list, written to `dest`,
// testing each against the list's hash map.
template <typename T, typename N>
inline PERFETTO_ALWAYS_INLINE void FilterInRangeScan(
    const CastFilterValueListResult& cast_result,
    Range range,
    const typename T::cpp_type* data,
    const BitVector* bv,
    const uint32_t* popcount,
    Span<uint32_t>& dest) {
  using HM =
      StorageType::VariantTypeAtIndex<T,
                                      CastFilterValueListResult::ValueHashMap>;
  // |data| is unused for Id columns (the storage index IS the value).
  base::ignore_result(data);
  const auto& hm = base::unchecked_get<HM>(cast_result.hash_map);
  uint32_t* write = dest.b;
  for (uint32_t i = range.b; i < range.e; ++i) {
    uint32_t si = IndexToStorageIndex<N>(i, bv, popcount);
    if (si == std::numeric_limits<uint32_t>::max()) {
      continue;
    }
    if constexpr (std::is_same_v<T, Id>) {
      if (hm.Find(CastFilterValueResult::Id{si}) != nullptr) {
        *write++ = i;
      }
    } else {
      if (hm.Find(data[si]) != nullptr) {
        *write++ = i;
      }
    }
  }
  dest.e = write;
}

// Copies the rows of `source` which lie in `range` to `out`, in order,
// returning the end of those written. `out` may be `source.b`.
inline PERFETTO_ALWAYS_INLINE uint32_t* CopySpanIntersectingRange(
    Span<const uint32_t> source,
    Range range,
    uint32_t* out) {
  for (const uint32_t* it = source.b; it != source.e; ++it) {
    *out = *it;
    out += (*it >= range.b && *it < range.e);
  }
  return out;
}

}  // namespace perfetto::trace_processor::core::filter

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_FILTER_KERNELS_H_
