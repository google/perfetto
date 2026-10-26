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

#include "src/trace_processor/core/common/filter_kernels.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/util/sorted_search.h"

namespace perfetto::trace_processor::core::filter {
namespace {

template <typename DataType>
auto SortedLowerBoundComparator(const StringPool* string_pool) {
  if constexpr (std::is_same_v<DataType, StringPool::Id>) {
    return comparators::StringComparator<Lt>{string_pool};
  } else {
    base::ignore_result(string_pool);
    return std::less<>();
  }
}

template <typename DataType>
auto SortedUpperBoundComparator(const StringPool* string_pool) {
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
      LowerBound<kCheapCompare<DataType>>(
          data + range.b, data + range.e, val,
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
      UpperBound<kCheapCompare<DataType>>(
          data + range.b, data + range.e, val,
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
  const DataType* eq_start = LowerBound<kCheapCompare<DataType>>(
      begin, end, val, SortedLowerBoundComparator<DataType>(string_pool));
  // Equal rows are usually few, so search from the first of them outwards.
  const DataType* eq_end =
      GallopingPartitionPoint(eq_start, end, [cmp_value](const DataType& v) {
        return std::equal_to<>()(v, cmp_value);
      });
  range.b = static_cast<uint32_t>(eq_start - data);
  range.e = static_cast<uint32_t>(eq_end - data);
}

Range SearchId(RangeOp op, uint32_t id, Range range) {
  switch (op.index()) {
    case RangeOp::GetTypeIndex<Eq>():
      IdEqualRange(id, range);
      return range;
    case RangeOp::GetTypeIndex<Lt>():
      IdBound(id, /*begin=*/false, range);
      return range;
    case RangeOp::GetTypeIndex<Le>():
      IdBound(id + 1, /*begin=*/false, range);
      return range;
    case RangeOp::GetTypeIndex<Gt>():
      IdBound(id + 1, /*begin=*/true, range);
      return range;
    case RangeOp::GetTypeIndex<Ge>():
      IdBound(id, /*begin=*/true, range);
      return range;
    default:
      PERFETTO_FATAL("Unsupported search operator");
  }
}

template <typename T>
Range SearchSortedOf(const SortedColumn& column,
                     RangeOp op,
                     const CastFilterValueResult::Value& value,
                     Range range) {
  using M = StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
  const auto* data = static_cast<const typename T::cpp_type*>(column.data);
  M val = base::unchecked_get<M>(value);
  switch (op.index()) {
    case RangeOp::GetTypeIndex<Eq>():
      SortedEqualRange(data, val, column.pool, range);
      return range;
    case RangeOp::GetTypeIndex<Lt>():
      range.e = SortedLowerBound(data, val, column.pool, range);
      return range;
    case RangeOp::GetTypeIndex<Le>():
      range.e = SortedUpperBound(data, val, column.pool, range);
      return range;
    case RangeOp::GetTypeIndex<Gt>():
      range.b = SortedUpperBound(data, val, column.pool, range);
      return range;
    case RangeOp::GetTypeIndex<Ge>():
      range.b = SortedLowerBound(data, val, column.pool, range);
      return range;
    default:
      PERFETTO_FATAL("Unsupported search operator");
  }
}

template <typename T, typename N>
Span<const uint32_t> SearchIndexEqualOf(
    const IndexColumn& column,
    const CastFilterValueResult::Value& value,
    Span<const uint32_t> entries) {
  using M = StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
  auto [b, e] = IndexEqualRange<T, N>(
      entries.b, entries.e, base::unchecked_get<M>(value),
      static_cast<const typename T::cpp_type*>(column.data), column.nulls,
      column.popcount, column.pool);
  return {b, e};
}

template <typename T>
Span<const uint32_t> SearchIndexEqualOf(
    const IndexColumn& column,
    const CastFilterValueResult::Value& value,
    Span<const uint32_t> entries) {
  switch (column.nullability.index()) {
    case Nullability::GetTypeIndex<NonNull>():
      return SearchIndexEqualOf<T, NonNull>(column, value, entries);
    case Nullability::GetTypeIndex<DenseNull>():
      return SearchIndexEqualOf<T, DenseNull>(column, value, entries);
    case Nullability::GetTypeIndex<SparseNull>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountUntilFinalization>():
      return SearchIndexEqualOf<T, SparseNull>(column, value, entries);
    default:
      PERFETTO_FATAL("Unsupported nullability");
  }
}

}  // namespace

Range SearchSorted(const SortedColumn& column,
                   RangeOp op,
                   const CastFilterValueResult::Value& value,
                   Range range) {
  switch (column.type.index()) {
    case StorageType::GetTypeIndex<Id>():
      return SearchId(
          op, base::unchecked_get<CastFilterValueResult::Id>(value).value,
          range);
    case StorageType::GetTypeIndex<Uint32>():
      return SearchSortedOf<Uint32>(column, op, value, range);
    case StorageType::GetTypeIndex<Int32>():
      return SearchSortedOf<Int32>(column, op, value, range);
    case StorageType::GetTypeIndex<Int64>():
      return SearchSortedOf<Int64>(column, op, value, range);
    case StorageType::GetTypeIndex<Double>():
      return SearchSortedOf<Double>(column, op, value, range);
    case StorageType::GetTypeIndex<String>():
      return SearchSortedOf<String>(column, op, value, range);
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

Span<const uint32_t> SearchIndexEqual(const IndexColumn& column,
                                      const CastFilterValueResult::Value& value,
                                      Span<const uint32_t> entries) {
  switch (column.type.index()) {
    case StorageType::GetTypeIndex<Uint32>():
      return SearchIndexEqualOf<Uint32>(column, value, entries);
    case StorageType::GetTypeIndex<Int32>():
      return SearchIndexEqualOf<Int32>(column, value, entries);
    case StorageType::GetTypeIndex<Int64>():
      return SearchIndexEqualOf<Int64>(column, value, entries);
    case StorageType::GetTypeIndex<Double>():
      return SearchIndexEqualOf<Double>(column, value, entries);
    case StorageType::GetTypeIndex<String>():
      return SearchIndexEqualOf<String>(column, value, entries);
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

}  // namespace perfetto::trace_processor::core::filter
