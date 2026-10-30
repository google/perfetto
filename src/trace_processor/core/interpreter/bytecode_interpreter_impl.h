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

#ifndef SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_INTERPRETER_IMPL_H_
#define SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_INTERPRETER_IMPL_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <type_traits>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/endian.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter_state.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter {

namespace ops {

// Handles invalid cast filter value results for filtering operations.
// If the cast result is invalid, updates the range or span accordingly.
//
// Returns true if the result is valid, false otherwise.
template <typename T>
PERFETTO_ALWAYS_INLINE bool HandleInvalidCastFilterValueResult(
    const filter::CastFilterValueResult::Validity& validity,
    T& update) {
  static_assert(std::is_same_v<T, Range> || std::is_same_v<T, Span<uint32_t>>);
  if (PERFETTO_UNLIKELY(validity != filter::CastFilterValueResult::kValid)) {
    if (validity == filter::CastFilterValueResult::kNoneMatch) {
      update.e = update.b;
    }
    return false;
  }
  return true;
}

inline PERFETTO_ALWAYS_INLINE void InitRange(InterpreterState& state,
                                             const struct InitRange& init) {
  using B = struct InitRange;
  state.WriteToRegister(init.arg<B::dest_register>(),
                        Range{0, init.arg<B::size>()});
}

inline PERFETTO_ALWAYS_INLINE void AllocateIndices(
    InterpreterState& state,
    const struct AllocateIndices& ai) {
  using B = struct AllocateIndices;

  if (auto* exist_slab =
          state.MaybeReadFromRegister(ai.arg<B::dest_slab_register>())) {
    // Ensure that the slab is at least as big as the requested size.
    PERFETTO_DCHECK(ai.arg<B::size>() <= exist_slab->size());

    // Update the span to point to the needed size of the slab.
    state.WriteToRegister(
        ai.arg<B::dest_span_register>(),
        Span<uint32_t>{exist_slab->begin(),
                       exist_slab->begin() + ai.arg<B::size>()});
  } else {
    auto slab = Slab<uint32_t>::Alloc(ai.arg<B::size>());
    Span<uint32_t> span{slab.begin(), slab.end()};
    state.WriteToRegister(ai.arg<B::dest_slab_register>(), std::move(slab));
    state.WriteToRegister(ai.arg<B::dest_span_register>(), span);
  }
}

inline PERFETTO_ALWAYS_INLINE void Iota(InterpreterState& state,
                                        const struct Iota& r) {
  using B = struct Iota;
  const auto& source = state.ReadFromRegister(r.arg<B::source_register>());
  auto& update = state.ReadFromRegister(r.arg<B::update_register>());
  PERFETTO_DCHECK(source.size() <= update.size());
  auto* end = update.b + source.size();
  std::iota(update.b, end, source.b);
  update.e = end;
}

inline PERFETTO_ALWAYS_INLINE void Reverse(InterpreterState& state,
                                           const struct Reverse& r) {
  using B = struct Reverse;
  auto& update = state.ReadFromRegister(r.arg<B::update_register>());
  std::reverse(update.b, update.e);
}

inline PERFETTO_ALWAYS_INLINE void StrideCopy(
    InterpreterState& state,
    const struct StrideCopy& stride_copy) {
  using B = struct StrideCopy;
  const auto& source =
      state.ReadFromRegister(stride_copy.arg<B::source_register>());
  auto& update = state.ReadFromRegister(stride_copy.arg<B::update_register>());
  uint32_t stride = stride_copy.arg<B::stride>();
  PERFETTO_DCHECK(source.size() * stride <= update.size());
  if (PERFETTO_LIKELY(stride == 1)) {
    memcpy(update.b, source.b, source.size() * sizeof(uint32_t));
  } else {
    uint32_t* write_ptr = update.b;
    for (const uint32_t* it = source.b; it < source.e; ++it) {
      *write_ptr = *it;
      write_ptr += stride;
    }
    PERFETTO_DCHECK(write_ptr == update.b + source.size() * stride);
  }
  update.e = update.b + source.size() * stride;
}

inline PERFETTO_ALWAYS_INLINE void PrefixPopcount(
    InterpreterState& state,
    const struct PrefixPopcount& popcount) {
  using B = struct PrefixPopcount;
  NullBitvector& nbv =
      state.ReadFromRegister(popcount.arg<B::null_bv_register>());
  if (nbv.popcount.size() > 0) {
    return;
  }
  nbv.popcount = nbv.bv->PrefixPopcount();
}

void AllocateRowLayoutBuffer(InterpreterState& state,
                             const struct AllocateRowLayoutBuffer& bytecode);

void LimitOffsetIndices(InterpreterState& state,
                        const LimitOffsetIndices& bytecode);

void CopySpanIntersectingRange(InterpreterState& state,
                               const CopySpanIntersectingRange& bytecode);

void InitRankMap(InterpreterState& state, const InitRankMap& bytecode);

void CollectIdIntoRankMap(InterpreterState& state,
                          const CollectIdIntoRankMap& bytecode);

void FinalizeRanksInMap(InterpreterState& state,
                        const FinalizeRanksInMap& bytecode);

void Distinct(InterpreterState& state, const Distinct& bytecode);

void SortRowLayout(InterpreterState& state, const SortRowLayout& bytecode);

void TranslateSparseNullIndices(InterpreterState& state,
                                const TranslateSparseNullIndices& bytecode);

void StrideTranslateAndCopySparseNullIndices(
    InterpreterState& state,
    const StrideTranslateAndCopySparseNullIndices& bytecode);

void StrideCopyDenseNullIndices(InterpreterState& state,
                                const StrideCopyDenseNullIndices& bytecode);

template <typename NullOp>
inline PERFETTO_ALWAYS_INLINE void NullFilter(InterpreterState& state,
                                              const NullFilterBase& filter) {
  using B = NullFilterBase;
  const NullBitvector& nbv =
      state.ReadFromRegister(filter.arg<B::null_bv_register>());
  auto& update = state.ReadFromRegister(filter.arg<B::update_register>());
  static constexpr bool kInvert = std::is_same_v<NullOp, IsNull>;
  update.e = nbv.bv->template PackLeft<kInvert>(update.b, update.e, update.b);
}

// Casts a filter value to the column's type and stores the result.
template <typename T, typename FilterValueFetcherImpl>
inline PERFETTO_ALWAYS_INLINE void CastFilterValue(
    InterpreterState& state,
    FilterValueFetcherImpl& fetcher,
    const CastFilterValueBase& f) {
  using B = CastFilterValueBase;
  state.WriteToRegister(
      f.arg<B::write_register>(),
      filter::CastFilterValue<T>(f.arg<B::fval_handle>().index, fetcher,
                                 f.arg<B::op>()));
}

template <typename T, typename Op>
inline PERFETTO_ALWAYS_INLINE void NonStringFilter(
    InterpreterState& state,
    const ::perfetto::trace_processor::core::interpreter::NonStringFilter<T,
                                                                          Op>&
        nf) {
  using B =
      ::perfetto::trace_processor::core::interpreter::NonStringFilter<T, Op>;
  const auto& value =
      state.ReadFromRegister(nf.template arg<B::val_register>());
  auto& update = state.ReadFromRegister(nf.template arg<B::update_register>());
  if (!HandleInvalidCastFilterValueResult(value.validity, update)) {
    return;
  }
  const auto& source =
      state.ReadFromRegister(nf.template arg<B::source_register>());
  using M =
      StorageType::VariantTypeAtIndex<T, filter::CastFilterValueResult::Value>;
  if constexpr (std::is_same_v<T, Id>) {
    update.e = filter::IdentityFilter(
        0, source.b, source.e, update.b,
        base::unchecked_get<M>(value.value).value,
        filter::comparators::IntegerOrDoubleComparator<uint32_t, Op>());
  } else if constexpr (IntegerOrDoubleType::Contains<T>()) {
    const auto* data = state.ReadStorageFromRegister<T>(
        nf.template arg<B::storage_register>());
    update.e = filter::Filter(
        data, source.b, source.e, update.b, base::unchecked_get<M>(value.value),
        filter::comparators::IntegerOrDoubleComparator<M, Op>());
  } else {
    static_assert(std::is_same_v<T, Id>, "Unsupported type");
  }
}

template <typename Op>
inline PERFETTO_ALWAYS_INLINE void StringFilter(InterpreterState& state,
                                                const StringFilterBase& sf) {
  using B = StringFilterBase;
  const auto& filter_value = state.ReadFromRegister(sf.arg<B::val_register>());
  auto& update = state.ReadFromRegister(sf.arg<B::update_register>());
  if (!HandleInvalidCastFilterValueResult(filter_value.validity, update)) {
    return;
  }
  const char* val = base::unchecked_get<const char*>(filter_value.value);
  const auto& source = state.ReadFromRegister(sf.arg<B::source_register>());
  const StringPool::Id* ptr =
      state.ReadStorageFromRegister<String>(sf.arg<B::storage_register>());
  update.e = filter::FilterStringCompare<Op>(state.string_pool, ptr, source.b,
                                             source.e, update.b, val);
}

// The comparison a search by R answers, writing `bound` of the range.
template <typename R>
PERFETTO_ALWAYS_INLINE core::RangeOp SortedFilterOp(BoundModifier bound) {
  if constexpr (std::is_same_v<R, EqualRange>) {
    PERFETTO_DCHECK(bound.Is<BothBounds>());
    return Eq{};
  } else if constexpr (std::is_same_v<R, LowerBound>) {
    return bound.Is<BeginBound>() ? core::RangeOp(Ge{}) : core::RangeOp(Lt{});
  } else {
    static_assert(std::is_same_v<R, UpperBound>, "Unsupported op");
    return bound.Is<BeginBound>() ? core::RangeOp(Gt{}) : core::RangeOp(Le{});
  }
}

template <typename T, typename RangeOp>
inline PERFETTO_ALWAYS_INLINE void SortedFilter(
    InterpreterState& state,
    const ::perfetto::trace_processor::core::interpreter::SortedFilter<T,
                                                                       RangeOp>&
        f) {
  using B =
      ::perfetto::trace_processor::core::interpreter::SortedFilter<T, RangeOp>;

  const auto& value = state.ReadFromRegister(f.template arg<B::val_register>());
  Range& update = state.ReadFromRegister(f.template arg<B::update_register>());
  if (!HandleInvalidCastFilterValueResult(value.validity, update)) {
    return;
  }
  BoundModifier bound = f.template arg<B::write_result_to>();
  if constexpr (std::is_same_v<T, Id>) {
    // An Id's row is its value: too little to be worth a call.
    uint32_t id =
        base::unchecked_get<filter::CastFilterValueResult::Id>(value.value)
            .value;
    if constexpr (std::is_same_v<RangeOp, EqualRange>) {
      filter::IdEqualRange(id, update);
    } else {
      filter::IdBound(id + std::is_same_v<RangeOp, UpperBound>,
                      bound.Is<BeginBound>(), update);
    }
  } else {
    filter::SortedColumn column{
        T{},
        state.ReadStorageFromRegister<T>(f.template arg<B::storage_register>()),
        state.string_pool};
    update = filter::SearchSorted(column, SortedFilterOp<RangeOp>(bound),
                                  value.value, update);
  }
}

template <typename T>
inline PERFETTO_ALWAYS_INLINE void LinearFilterEq(
    InterpreterState& state,
    const ::perfetto::trace_processor::core::interpreter::LinearFilterEq<T>&
        leq) {
  using B = ::perfetto::trace_processor::core::interpreter::LinearFilterEq<T>;

  Span<uint32_t>& span =
      state.ReadFromRegister(leq.template arg<B::update_register>());
  Range range = state.ReadFromRegister(leq.template arg<B::source_register>());
  PERFETTO_DCHECK(range.size() <= span.size());

  const auto& res =
      state.ReadFromRegister(leq.template arg<B::filter_value_reg>());
  if (!HandleInvalidCastFilterValueResult(res.validity, range)) {
    std::iota(span.b, span.b + range.size(), range.b);
    span.e = span.b + range.size();
    return;
  }

  const auto* data =
      state.ReadStorageFromRegister<T>(leq.template arg<B::storage_register>());

  using Compare = std::remove_cv_t<std::remove_reference_t<decltype(*data)>>;
  using M =
      StorageType::VariantTypeAtIndex<T, filter::CastFilterValueResult::Value>;
  const auto& value = base::unchecked_get<M>(res.value);
  Compare to_compare;
  if constexpr (std::is_same_v<T, String>) {
    auto id = state.string_pool->GetId(value);
    if (!id) {
      span.e = span.b;
      return;
    }
    to_compare = *id;
  } else {
    to_compare = value;
  }

  // Note to future readers: this can be optimized further with explicit SIMD
  // but the compiler does a pretty good job even without it. For context,
  // we're talking about query changing from 2s -> 1.6s on a 12m row table.
  uint32_t* o_write = span.b;
  for (uint32_t i = range.b; i < range.e; ++i) {
    if (std::equal_to<>()(data[i], to_compare)) {
      *o_write++ = i;
    }
  }
  span.e = o_write;
}

// The null bitvector and popcount an index search reads, or nulls for a
// column without one.
inline std::pair<const BitVector*, const uint32_t*> NullsFor(
    const NullBitvector* nbv) {
  return nbv ? std::make_pair(nbv->bv, nbv->popcount.data())
             : std::make_pair(nullptr, nullptr);
}

template <typename T, typename N>
inline PERFETTO_ALWAYS_INLINE void IndexedFilterEq(
    InterpreterState& state,
    const IndexedFilterEqBase& bytecode) {
  using B = IndexedFilterEqBase;
  const auto& filter_value =
      state.ReadFromRegister(bytecode.arg<B::filter_value_reg>());
  const auto& source =
      state.ReadFromRegister(bytecode.arg<B::source_register>());
  Span<uint32_t> dest(source.b, source.e);
  if (!HandleInvalidCastFilterValueResult(filter_value.validity, dest)) {
    state.WriteToRegister(bytecode.arg<B::dest_register>(), dest);
    return;
  }
  const auto* data =
      state.ReadStorageFromRegister<T>(bytecode.arg<B::storage_register>());
  const NullBitvector* nbv =
      state.MaybeReadFromRegister(bytecode.arg<B::null_bv_register>());

  auto [bv, popcount] = NullsFor(nbv);
  filter::IndexColumn column{T{}, N{}, data, bv, popcount, state.string_pool};
  // The search only narrows the range, so the entries are still ours to write.
  Span<const uint32_t> found = filter::SearchIndexEqual(
      column, filter_value.value, Span<const uint32_t>(dest.b, dest.e));
  dest = Span<uint32_t>(const_cast<uint32_t*>(found.b),
                        const_cast<uint32_t*>(found.e));
  state.WriteToRegister(bytecode.arg<B::dest_register>(), dest);
}

// ============================================================================
// FilterIn: IN-clause filtering
//
// This section implements the FilterIn bytecode which handles SQL IN(...)
// clauses. The code is organized as follows:
//
//   1. Constants (thresholds for algorithm selection)
//   2. Helpers for building lookup structures (MaybeBuildBitVector,
//      MaybeBuildValueList)
//   3. CastFilterValueList: casts raw filter values into typed lookup
//      structures (HashMap, BitVector, sorted ValueList)
//   4. NonIndexedFilterInImpl: scans indices with hash/bitvector/linear lookup
//   5. IndexedFilterInBinarySearch: binary-searches a sorted index
//   6. IndexedFilterIn: dispatches between binary search and linear scan
//   7. FilterIn: the public bytecode entry point
// ============================================================================

// Casts raw filter values into typed lookup structures for IN-clause
// filtering. Populates a HashMap (canonical), and optionally a BitVector
// (for dense Id/Uint32) and a sorted ValueList (for small lists).
//
// Reuses existing allocations when a previous result is present in the
// register, avoiding repeated heap allocation across query iterations.
template <typename T, typename FilterValueFetcherImpl>
inline PERFETTO_ALWAYS_INLINE void CastFilterValueList(
    InterpreterState& state,
    FilterValueFetcherImpl& fetcher,
    const CastFilterValueListBase& c) {
  using B = CastFilterValueListBase;
  FilterValueHandle handle = c.arg<B::fval_handle>();

  // Reuse existing allocation if available, otherwise allocate fresh.
  filter::CastFilterValueListResult::Ptr* existing =
      state.MaybeReadFromRegister(c.arg<B::write_register>());
  filter::CastFilterValueListResult::Ptr result;
  if (existing && *existing) {
    result = std::move(*existing);
    result->Clear<T>();
  } else {
    result = std::make_unique<filter::CastFilterValueListResult>();
    result->Init<T>();
  }
  filter::CastFilterValueList<T>(handle.index, fetcher, c.arg<B::op>(),
                                 state.string_pool, *result);
  state.WriteToRegister(c.arg<B::write_register>(), std::move(result));
}

// Attempts the indexed binary search path for FilterIn. Looks up the
// index register; if present and the IN list is small enough, binary-
// searches the sorted index for each value and populates dest. Returns
// true if binary search was performed, false if the caller should use
// a different strategy.
template <typename T, typename N>
inline PERFETTO_ALWAYS_INLINE bool TryIndexedFilterInBinarySearch(
    InterpreterState& state,
    const FilterInBase& bytecode,
    const filter::CastFilterValueListResult& cast_result,
    Span<uint32_t>& dest) {
  using B = FilterInBase;

  // Id columns have no backing storage and are never indexed.
  if constexpr (std::is_same_v<T, Id>) {
    return false;
  } else {
    const Span<uint32_t>* index =
        state.MaybeReadFromRegister(bytecode.arg<B::index_register>());
    if (!index) {
      return false;
    }

    const auto* data =
        state.ReadStorageFromRegister<T>(bytecode.arg<B::storage_register>());
    auto [bv, popcount] = NullsFor(
        state.MaybeReadFromRegister(bytecode.arg<B::null_bv_register>()));
    return filter::IndexedFilterInBinarySearch<T, N>(
        cast_result, *index, data, bv, popcount, state.string_pool, dest);
  }
}

// Scan path for FilterIn when an index is present but the IN list is too
// large for binary search. The index is ignored entirely: we iterate over
// the source range [b, e), check each row against the hash map, and write
// matching row indices to dest.
template <typename T, typename N>
inline PERFETTO_ALWAYS_INLINE void IndexedFilterInRangeScan(
    InterpreterState& state,
    const FilterInBase& bytecode,
    const filter::CastFilterValueListResult& cast_result,
    Span<uint32_t>& dest) {
  using B = FilterInBase;
  const auto* data =
      state.ReadStorageFromRegister<T>(bytecode.arg<B::storage_register>());
  auto [bv, popcount] = NullsFor(
      state.MaybeReadFromRegister(bytecode.arg<B::null_bv_register>()));
  const Range& range =
      state.ReadFromRegister(bytecode.arg<B::source_range_register>());
  filter::FilterInRangeScan<T, N>(cast_result, range, data, bv, popcount, dest);
}

// Unified filter-in bytecode entry point.
template <typename T, typename N>
inline PERFETTO_ALWAYS_INLINE void FilterIn(InterpreterState& state,
                                            const FilterInBase& bytecode) {
  using B = FilterInBase;
  const auto& cast_result =
      *state.ReadFromRegister(bytecode.arg<B::value_list_register>());
  auto& dest = state.ReadFromRegister(bytecode.arg<B::dest_register>());

  if (!HandleInvalidCastFilterValueResult(cast_result.validity, dest)) {
    return;
  }

  // Try indexed binary search for small IN lists on indexed columns.
  if (TryIndexedFilterInBinarySearch<T, N>(state, bytecode, cast_result,
                                           dest)) {
    return;
  }

  // Scan path: either indexed range scan or non-indexed span scan.
  const auto* source_range =
      state.MaybeReadFromRegister(bytecode.arg<B::source_range_register>());
  if (source_range) {
    // Index present but IN list too large for binary search. Ignore the
    // index entirely and scan the row range with hash lookup.
    IndexedFilterInRangeScan<T, N>(state, bytecode, cast_result, dest);
    return;
  }
  // Non-indexed path: scan source span with in-place compaction of dest.
  const auto* data =
      state.ReadStorageFromRegister<T>(bytecode.arg<B::storage_register>());
  const Span<uint32_t>& source =
      state.ReadFromRegister(bytecode.arg<B::source_register>());
  filter::NonIndexedFilterInScan<T>(cast_result, data, 0, source, dest);
}

// ============================================================================
// End FilterIn
// ============================================================================

inline PERFETTO_ALWAYS_INLINE void Uint32SetIdSortedEq(
    InterpreterState& state,
    const Uint32SetIdSortedEq& bytecode) {
  using B = struct Uint32SetIdSortedEq;

  const filter::CastFilterValueResult& cast_result =
      state.ReadFromRegister(bytecode.arg<B::val_register>());
  auto& update = state.ReadFromRegister(bytecode.arg<B::update_register>());
  if (!HandleInvalidCastFilterValueResult(cast_result.validity, update)) {
    return;
  }
  using ValueType =
      StorageType::VariantTypeAtIndex<Uint32,
                                      filter::CastFilterValueResult::Value>;
  auto val = base::unchecked_get<ValueType>(cast_result.value);
  const auto* storage = state.ReadStorageFromRegister<Uint32>(
      bytecode.arg<B::storage_register>());
  filter::SetIdSortedEqualRange(storage, val, update);
}

inline PERFETTO_ALWAYS_INLINE void SpecializedStorageSmallValueEq(
    InterpreterState& state,
    const SpecializedStorageSmallValueEq& bytecode) {
  using B = struct SpecializedStorageSmallValueEq;

  const filter::CastFilterValueResult& cast_result =
      state.ReadFromRegister(bytecode.arg<B::val_register>());
  auto& update = state.ReadFromRegister(bytecode.arg<B::update_register>());
  if (!HandleInvalidCastFilterValueResult(cast_result.validity, update)) {
    return;
  }
  using ValueType =
      StorageType::VariantTypeAtIndex<Uint32,
                                      filter::CastFilterValueResult::Value>;
  auto val = base::unchecked_get<ValueType>(cast_result.value);
  const BitVector* bv =
      state.ReadFromRegister(bytecode.arg<B::small_value_bv_register>());
  const Span<const uint32_t>& popcount =
      state.ReadFromRegister(bytecode.arg<B::small_value_popcount_register>());

  filter::SmallValueEqualRange(*bv, popcount.b, val, update);
}

template <typename T, typename Nullability>
inline PERFETTO_ALWAYS_INLINE void CopyToRowLayout(
    InterpreterState& state,
    const CopyToRowLayoutBase& bytecode) {
  using B = CopyToRowLayoutBase;

  const auto& source =
      state.ReadFromRegister(bytecode.arg<B::source_indices_register>());
  bool invert = bytecode.arg<B::invert_copied_bits>();

  auto& dest_buffer =
      state.ReadFromRegister(bytecode.arg<B::dest_buffer_register>());

  const auto* data =
      state.ReadStorageFromRegister<T>(bytecode.arg<B::storage_register>());

  // GCC complains that these variables are not used in the NonNull branches.
  [[maybe_unused]] const StringIdToRankMap* rank_map_ptr =
      state.MaybeReadFromRegister(bytecode.arg<B::rank_map_register>());
  [[maybe_unused]] const NullBitvector* nbv =
      state.MaybeReadFromRegister(bytecode.arg<B::null_bv_register>());

  // False if the row is null.
  auto storage_index = [&](uint32_t i, uint32_t* out) {
    uint32_t table_index = source.b[i];
    if constexpr (std::is_same_v<Nullability, NonNull>) {
      *out = table_index;
      return true;
    } else if constexpr (std::is_same_v<Nullability, SparseNull>) {
      PERFETTO_DCHECK(nbv && nbv->popcount.size() > 0);
      if (!nbv->bv->is_set(table_index)) {
        return false;
      }
      *out = static_cast<uint32_t>(
          nbv->popcount[table_index / 64] +
          nbv->bv->count_set_bits_until_in_word(table_index));
      return true;
    } else if constexpr (std::is_same_v<Nullability, DenseNull>) {
      *out = table_index;
      return nbv->bv->is_set(table_index);
    } else {
      static_assert(std::is_same_v<Nullability, NonNull>,
                    "Unsupported Nullability type");
    }
  };
  RowLayout::Slot slot{bytecode.arg<B::row_layout_offset>(),
                       bytecode.arg<B::row_layout_stride>(),
                       !std::is_same_v<Nullability, NonNull>, invert};
  auto count = static_cast<uint32_t>(source.size());
  if constexpr (std::is_same_v<T, Id>) {
    RowLayout::Write<uint32_t>(slot, count, storage_index, dest_buffer.data());
  } else if constexpr (std::is_same_v<T, String>) {
    RowLayout::Write<uint32_t>(
        slot, count,
        [&](uint32_t i, uint32_t* out) {
          uint32_t index;
          if (!storage_index(i, &index)) {
            return false;
          }
          if (rank_map_ptr) {
            const uint32_t* rank = (*rank_map_ptr)->Find(data[index]);
            PERFETTO_DCHECK(rank);
            *out = *rank;
          } else {
            *out = data[index].raw_id();
          }
          return true;
        },
        dest_buffer.data());
  } else {
    using V = std::remove_cv_t<std::remove_reference_t<decltype(*data)>>;
    RowLayout::Write<V>(
        slot, count,
        [&](uint32_t i, V* out) {
          uint32_t index;
          if (!storage_index(i, &index)) {
            return false;
          }
          *out = data[index];
          return true;
        },
        dest_buffer.data());
  }
}

template <typename T, typename Op>
inline PERFETTO_ALWAYS_INLINE void FindMinMaxIndex(
    InterpreterState& state,
    const FindMinMaxIndex<T, Op>& bytecode) {
  using B = FindMinMaxIndexBase;
  auto& indices =
      state.ReadFromRegister(bytecode.template arg<B::update_register>());
  if (indices.empty()) {
    return;
  }

  const auto* data = state.ReadStorageFromRegister<T>(
      bytecode.template arg<B::storage_register>());
  auto get_value = [&](uint32_t idx) {
    if constexpr (std::is_same_v<T, Id>) {
      base::ignore_result(data);
      return idx;
    } else if constexpr (std::is_same_v<T, String>) {
      return state.string_pool->Get(data[idx]);
    } else {
      return data[idx];
    }
  };
  uint32_t best_idx = *indices.b;
  auto best_val = get_value(best_idx);
  for (const uint32_t* it = indices.b + 1; it != indices.e; ++it) {
    uint32_t current_idx = *it;
    auto current_val = get_value(current_idx);
    bool current_is_better;
    if constexpr (std::is_same_v<Op, MinOp>) {
      current_is_better = current_val < best_val;
    } else {
      current_is_better = current_val > best_val;
    }
    if (current_is_better) {
      best_idx = current_idx;
      best_val = current_val;
    }
  }
  *indices.b = best_idx;
  indices.e = indices.b + 1;
}

}  // namespace ops

// Macros for generating case statements that dispatch to ops:: free functions.
// Uses __VA_ARGS__ to handle template types with commas (e.g., SortedFilter<Id,
// EqualRange>).

// For ops that need state and fetcher:
#define PERFETTO_OP_CASE_FVF(...)                                \
  case base::variant_index<BytecodeVariant, __VA_ARGS__>(): {    \
    ops::__VA_ARGS__(state_, fetcher,                            \
                     static_cast<const __VA_ARGS__&>(bytecode)); \
    break;                                                       \
  }

// For ops that only need state:
#define PERFETTO_OP_CASE_STATE(...)                                      \
  case base::variant_index<BytecodeVariant, __VA_ARGS__>(): {            \
    ops::__VA_ARGS__(state_, static_cast<const __VA_ARGS__&>(bytecode)); \
    break;                                                               \
  }

// Executes all bytecode instructions using free functions.
template <typename FilterValueFetcherImpl>
PERFETTO_ALWAYS_INLINE void Interpreter<FilterValueFetcherImpl>::Execute(
    FilterValueFetcherImpl& fetcher) {
  for (const auto& bytecode : state_.bytecode) {
    switch (bytecode.option) {
      PERFETTO_DATAFRAME_BYTECODE_FVF_LIST(PERFETTO_OP_CASE_FVF)
      PERFETTO_DATAFRAME_BYTECODE_STATE_ONLY_LIST(PERFETTO_OP_CASE_STATE)
      default:
        PERFETTO_ASSUME(false);
    }
  }
}

#undef PERFETTO_OP_CASE_FVF
#undef PERFETTO_OP_CASE_STATE

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_INTERPRETER_IMPL_H_
