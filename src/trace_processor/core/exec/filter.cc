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

#include "src/trace_processor/core/exec/filter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueListResult;
using filter::CastFilterValueResult;

// The batch rows kept so far: `count` of them from `rows`, increasing. Each
// condition narrows them in place.
struct Rows {
  uint32_t* rows;
  uint32_t count;
};

// Keeps the rows which are null if `nulls`, or which are not otherwise.
void KeepNulls(const ColumnView& column, bool nulls, Rows& r) {
  const BitVector* validity = column.validity();
  if (!validity) {
    r.count = nulls ? 0 : r.count;
    return;
  }
  uint32_t start = column.start();
  uint32_t kept = 0;
  for (uint32_t i = 0; i < r.count; ++i) {
    uint32_t row = r.rows[i];
    if (validity->is_set(start + row) != nulls) {
      r.rows[kept++] = row;
    }
  }
  r.count = kept;
}

// Keeps the rows of a column of storage type T which compare true by O with
// the filter value. The kernels read each row's value at the column's data
// plus the row, so the data passed starts where the window does.
template <typename T, typename O>
void Compare(const ColumnView& column,
             const FilterValue& cast,
             const StringPool& pool,
             Rows& r) {
  uint32_t* begin = r.rows;
  uint32_t* end = r.rows + r.count;
  uint32_t* kept;
  if constexpr (std::is_same_v<T, Id>) {
    uint32_t value =
        base::unchecked_get<CastFilterValueResult::Id>(cast.value.value).value;
    auto cmp = filter::comparators::IntegerOrDoubleComparator<uint32_t, O>();
    if (column.kind() == ColumnView::Kind::kSequence) {
      // An Id's value is where it would be stored.
      uint32_t start = column.start();
      kept = filter::IdentityFilter(begin, end, begin, value,
                                    [start, cmp](uint32_t row, uint32_t v) {
                                      return cmp(start + row, v);
                                    });
    } else {
      // Copied out, an Id column stores its values.
      const auto* data =
          static_cast<const uint32_t*>(column.data()) + column.start();
      kept = filter::Filter(data, begin, end, begin, value, cmp);
    }
  } else if constexpr (std::is_same_v<T, String>) {
    const auto* data =
        static_cast<const StringPool::Id*>(column.data()) + column.start();
    if constexpr (std::is_same_v<O, Eq> || std::is_same_v<O, Ne>) {
      // Equal strings are the same string in the pool.
      static_assert(sizeof(StringPool::Id) == sizeof(uint32_t));
      using Cmp = std::conditional_t<std::is_same_v<O, Eq>, std::equal_to<>,
                                     std::not_equal_to<>>;
      kept = filter::Filter(reinterpret_cast<const uint32_t*>(data), begin, end,
                            begin, cast.string_id.raw_id(), Cmp());
    } else {
      kept = filter::FilterStringCompare<O>(&pool, data, begin, end, begin,
                                            cast.text.c_str());
    }
  } else {
    using M = StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    const auto* data = static_cast<const M*>(column.data()) + column.start();
    kept = filter::Filter(
        data, begin, end, begin, base::unchecked_get<M>(cast.value.value),
        filter::comparators::IntegerOrDoubleComparator<M, O>());
  }
  r.count = static_cast<uint32_t>(kept - begin);
}

template <typename T>
void CompareByOp(const Op& op,
                 const ColumnView& column,
                 const FilterValue& cast,
                 const StringPool& pool,
                 Rows& r) {
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return Compare<T, Eq>(column, cast, pool, r);
    case Op::GetTypeIndex<Ne>():
      return Compare<T, Ne>(column, cast, pool, r);
    case Op::GetTypeIndex<Lt>():
      return Compare<T, Lt>(column, cast, pool, r);
    case Op::GetTypeIndex<Le>():
      return Compare<T, Le>(column, cast, pool, r);
    case Op::GetTypeIndex<Gt>():
      return Compare<T, Gt>(column, cast, pool, r);
    case Op::GetTypeIndex<Ge>():
      return Compare<T, Ge>(column, cast, pool, r);
    default:
      PERFETTO_FATAL("Unsupported filter operator");
  }
}

// Applies a comparison to the rows kept so far. As the interpreter does, it
// first drops the nulls, which it can never be true of, and only then looks at
// whether its value cast.
void ApplyComparison(const Op& op,
                     const FilterValue& cast,
                     const ColumnView& column,
                     const StringPool& pool,
                     Rows& r) {
  switch (cast.value.validity) {
    case CastFilterValueResult::kNoneMatch:
      r.count = 0;
      return;
    case CastFilterValueResult::kAllMatch:
      KeepNulls(column, /*nulls=*/false, r);
      return;
    case CastFilterValueResult::kValid:
      KeepNulls(column, /*nulls=*/false, r);
      break;
  }
  if (r.count == 0) {
    return;
  }
  // The value was cast for the column's storage type, which picks its
  // alternative.
  PERFETTO_DCHECK(cast.value.value.index() == column.type().index());
  switch (column.type().index()) {
    case StorageType::GetTypeIndex<Id>():
      return CompareByOp<Id>(op, column, cast, pool, r);
    case StorageType::GetTypeIndex<Uint32>():
      return CompareByOp<Uint32>(op, column, cast, pool, r);
    case StorageType::GetTypeIndex<Int32>():
      return CompareByOp<Int32>(op, column, cast, pool, r);
    case StorageType::GetTypeIndex<Int64>():
      return CompareByOp<Int64>(op, column, cast, pool, r);
    case StorageType::GetTypeIndex<Double>():
      return CompareByOp<Double>(op, column, cast, pool, r);
    case StorageType::GetTypeIndex<String>():
      return CompareByOp<String>(op, column, cast, pool, r);
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

// Keeps the rows of a column of storage type T holding one of the values of
// `list`, with the interpreter's kernels.
template <typename T>
void CompareIn(const ColumnView& column,
               const CastFilterValueListResult& list,
               Rows& r) {
  uint32_t* begin = r.rows;
  uint32_t* end = r.rows + r.count;
  uint32_t start = column.start();
  if constexpr (std::is_same_v<T, Id>) {
    // An Id's value is where it would be stored, or once copied out, stored.
    const auto* data = static_cast<const uint32_t*>(column.data());
    const auto& set = base::unchecked_get<
        CastFilterValueListResult::HashMap<CastFilterValueResult::Id>>(
        list.hash_map);
    const BitVector& dense = list.bit_vector;
    uint32_t kept = 0;
    for (uint32_t* it = begin; it != end; ++it) {
      uint32_t index = start + *it;
      uint32_t value = data ? data[index] : index;
      bool in = dense.size() > 0
                    ? value < dense.size() && dense.is_set(value)
                    : set.Find(CastFilterValueResult::Id{value}) != nullptr;
      if (in) {
        r.rows[kept++] = *it;
      }
    }
    r.count = kept;
  } else {
    using M =
        StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::Value>;
    const auto* data = static_cast<const M*>(column.data()) + start;
    Span<uint32_t> source(begin, end);
    Span<uint32_t> dest(begin, end);
    filter::NonIndexedFilterInScan<T>(list, data, source, dest);
    r.count = static_cast<uint32_t>(dest.e - begin);
  }
}

// Applies an IN to the rows kept so far. As a comparison does, it first drops
// the nulls, which it can never be true of. A list never cast keeps nothing.
void ApplyIn(const FilterValue& cast, const ColumnView& column, Rows& r) {
  if (!cast.list) {
    r.count = 0;
    return;
  }
  const CastFilterValueListResult& list = *cast.list;
  switch (list.validity) {
    case CastFilterValueResult::kNoneMatch:
      r.count = 0;
      return;
    case CastFilterValueResult::kAllMatch:
      KeepNulls(column, /*nulls=*/false, r);
      return;
    case CastFilterValueResult::kValid:
      KeepNulls(column, /*nulls=*/false, r);
      break;
  }
  if (r.count == 0) {
    return;
  }
  switch (column.type().index()) {
    case StorageType::GetTypeIndex<Id>():
      return CompareIn<Id>(column, list, r);
    case StorageType::GetTypeIndex<Uint32>():
      return CompareIn<Uint32>(column, list, r);
    case StorageType::GetTypeIndex<Int32>():
      return CompareIn<Int32>(column, list, r);
    case StorageType::GetTypeIndex<Int64>():
      return CompareIn<Int64>(column, list, r);
    case StorageType::GetTypeIndex<Double>():
      return CompareIn<Double>(column, list, r);
    case StorageType::GetTypeIndex<String>():
      return CompareIn<String>(column, list, r);
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

}  // namespace

Filter::Filter(std::vector<Condition> conditions, const StringPool* pool)
    : conditions_(std::move(conditions)), pool_(pool) {
  for (const Condition& c : conditions_) {
    PERFETTO_CHECK(c.op.IsAnyOf<NonStringOp>() || c.op.IsAnyOf<NullOp>() ||
                   c.op.Is<In>());
  }
}

Filter::~Filter() = default;
Filter::State::~State() = default;

std::unique_ptr<OperatorState> Filter::MakeState(Context& context) const {
  return std::make_unique<State>(context);
}

bool Filter::Process(RowBatch& batch, OperatorState& state) const {
  State& s = state.Cast<State>();
  const FilterValue* values = s.context->filter_values().b;
  const Selection& selection = batch.selection();
  Rows r{s.rows.data(), selection.size()};
  for (uint32_t i = 0; i < r.count; ++i) {
    r.rows[i] = selection[i];
  }
  for (uint32_t i = 0; i < conditions_.size() && r.count != 0; ++i) {
    const Condition& c = conditions_[i];
    const ColumnView& column = batch.column(c.column);
    PERFETTO_DCHECK(column.kind() != ColumnView::Kind::kVariant);
    if (c.op.IsAnyOf<NullOp>()) {
      KeepNulls(column, c.op.Is<IsNull>(), r);
      continue;
    }
    PERFETTO_DCHECK(c.value < s.context->filter_values().size());
    if (c.op.Is<In>()) {
      ApplyIn(values[c.value], column, r);
      continue;
    }
    ApplyComparison(c.op, values[c.value], column, *pool_, r);
  }
  batch.mutable_selection().KeepRows(
      Span<const uint32_t>(r.rows, r.rows + r.count));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
