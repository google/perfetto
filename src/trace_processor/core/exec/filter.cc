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
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueListResult;
using filter::CastFilterValueResult;

// Hands a condition's values to the filter casts, as the dataframe module
// hands them SQLite's. There is one value, the condition's first, or for an
// IN, the list of all of them to iterate.
struct ConditionValues : ValueFetcher {
  using Type = int;
  static constexpr Type kInt64 =
      static_cast<Type>(base::variant_index<Filter::Value, int64_t>());
  static constexpr Type kDouble =
      static_cast<Type>(base::variant_index<Filter::Value, double>());
  static constexpr Type kString =
      static_cast<Type>(base::variant_index<Filter::Value, std::string>());
  // A condition never holds a null, but the casts name the type.
  static constexpr Type kNull = kString + 1;

  explicit ConditionValues(const std::vector<Filter::Value>& v) : values(v) {}

  int64_t GetInt64Value(uint32_t) const {
    return std::get<int64_t>(values[current]);
  }
  double GetDoubleValue(uint32_t) const {
    return std::get<double>(values[current]);
  }
  const char* GetStringValue(uint32_t) const {
    return std::get<std::string>(values[current]).c_str();
  }
  Type GetValueType(uint32_t) const {
    return static_cast<Type>(values[current].index());
  }
  bool IteratorInit(uint32_t) {
    current = 0;
    return !values.empty();
  }
  bool IteratorNext(uint32_t) { return ++current < values.size(); }

  const std::vector<Filter::Value>& values;
  size_t current = 0;
};

// The rows kept so far, and where each one's value is stored in the column.
struct Rows {
  uint32_t* rows;
  uint32_t* storage;
  uint32_t count;
};

// Keeps the rows which are null if `nulls`, or not null otherwise.
void KeepNulls(const ColumnView& column, bool nulls, Rows& r) {
  const BitVector* validity = column.validity();
  if (!validity) {
    r.count = nulls ? 0 : r.count;
    return;
  }
  uint32_t kept = 0;
  for (uint32_t i = 0; i < r.count; ++i) {
    if (validity->is_set(r.storage[i]) != nulls) {
      r.rows[kept] = r.rows[i];
      r.storage[kept] = r.storage[i];
      ++kept;
    }
  }
  r.count = kept;
}

// As a dataframe handles a value which did not cast: no row matches, or every
// (non-null) row does. Returns whether the value did cast.
bool HandleCast(CastFilterValueResult::Validity validity, Rows& r) {
  if (validity == CastFilterValueResult::kNoneMatch) {
    r.count = 0;
  }
  return validity == CastFilterValueResult::kValid;
}

// The kernels write only the rows kept, so where each is stored is found
// again from the row.
void Refresh(const ColumnView& column, Rows& r, const uint32_t* end) {
  r.count = static_cast<uint32_t>(end - r.rows);
  for (uint32_t i = 0; i < r.count; ++i) {
    r.storage[i] = column.selection().GetIndex(r.rows[i]);
  }
}

// A comparison with one value, of a column of storage type T.
template <typename T, typename O>
void CompareOne(const ColumnView& column,
                const std::vector<Filter::Value>& values,
                const StringPool& pool,
                Rows& r) {
  ConditionValues fetcher(values);
  auto type = fetcher.GetValueType(0);
  const uint32_t* begin = r.storage;
  const uint32_t* end = r.storage + r.count;
  if constexpr (std::is_same_v<T, Id>) {
    uint32_t value;
    auto validity = filter::CastFilterValueToInteger<uint32_t>(
        0, type, fetcher, NonStringOp(O{}), value);
    if (HandleCast(validity, r)) {
      Refresh(
          column, r,
          filter::IdentityFilter(
              begin, end, r.rows, value,
              filter::comparators::IntegerOrDoubleComparator<uint32_t, O>()));
    }
  } else if constexpr (std::is_same_v<T, String>) {
    const char* value;
    auto validity =
        filter::CastFilterValueToString(0, type, fetcher, StringOp(O{}), value);
    if (HandleCast(validity, r)) {
      const auto* data = static_cast<const StringPool::Id*>(column.data());
      Refresh(column, r,
              filter::FilterStringCompare<O>(&pool, data, begin, end, r.rows,
                                             value));
    }
  } else {
    using M = StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    M value;
    auto validity = filter::CastFilterValueToIntegerOrDouble<M>(
        0, type, fetcher, NonStringOp(O{}), value);
    if (HandleCast(validity, r)) {
      const auto* data = static_cast<const M*>(column.data());
      Refresh(column, r,
              filter::Filter(
                  data, begin, end, r.rows, value,
                  filter::comparators::IntegerOrDoubleComparator<M, O>()));
    }
  }
}

// An IN over a column of storage type T, as a dataframe runs one without an
// index.
template <typename T>
void CompareIn(const ColumnView& column,
               const std::vector<Filter::Value>& values,
               const StringPool& pool,
               Rows& r,
               Filter::CastList& cast) {
  constexpr uint32_t kType = StorageType::GetTypeIndex<T>() + 1;
  CastFilterValueListResult& list = cast.list;
  if (PERFETTO_UNLIKELY(!cast.fresh || cast.type != kType)) {
    if (cast.type == kType) {
      list.Clear<T>();
    } else {
      list.Init<T>();
    }
    ConditionValues fetcher(values);
    filter::CastFilterValueList<T>(0, fetcher, NonNullOp(Eq{}), &pool, list);
    cast.type = kType;
    cast.fresh = true;
  }
  if (!HandleCast(list.validity, r)) {
    return;
  }
  Span<uint32_t> source(r.storage, r.storage + r.count);
  Span<uint32_t> dest(r.rows, r.rows + r.count);
  if constexpr (std::is_same_v<T, Id>) {
    // An Id's value is where it is stored, so it has no data.
    filter::NonIndexedFilterInScan<T>(
        list, static_cast<const uint32_t*>(nullptr), source, dest);
  } else {
    using D = std::conditional_t<
        std::is_same_v<T, String>, StringPool::Id,
        StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>>;
    filter::NonIndexedFilterInScan<T>(
        list, static_cast<const D*>(column.data()), source, dest);
  }
  Refresh(column, r, dest.e);
}

template <typename T>
void Compare(const Filter::Condition& condition,
             const ColumnView& column,
             const StringPool& pool,
             Rows& r,
             Filter::CastList& cast) {
  const auto& v = condition.values;
  switch (condition.op.index()) {
    case Op::GetTypeIndex<Eq>():
      return CompareOne<T, Eq>(column, v, pool, r);
    case Op::GetTypeIndex<Ne>():
      return CompareOne<T, Ne>(column, v, pool, r);
    case Op::GetTypeIndex<Lt>():
      return CompareOne<T, Lt>(column, v, pool, r);
    case Op::GetTypeIndex<Le>():
      return CompareOne<T, Le>(column, v, pool, r);
    case Op::GetTypeIndex<Gt>():
      return CompareOne<T, Gt>(column, v, pool, r);
    case Op::GetTypeIndex<Ge>():
      return CompareOne<T, Ge>(column, v, pool, r);
    case Op::GetTypeIndex<In>():
      return CompareIn<T>(column, v, pool, r, cast);
    default:
      PERFETTO_FATAL("Unsupported filter operator");
  }
}

// Applies one condition to the rows kept so far. As a dataframe does, a
// comparison first drops the nulls, which it can never be true of.
void Apply(const Filter::Condition& condition,
           const ColumnView& column,
           const StringPool& pool,
           Rows& r,
           Filter::CastList& cast) {
  PERFETTO_DCHECK(column.kind() != ColumnView::Kind::kVariant);
  if (condition.op.Is<IsNull>() || condition.op.Is<IsNotNull>()) {
    KeepNulls(column, condition.op.Is<IsNull>(), r);
    return;
  }
  KeepNulls(column, /*nulls=*/false, r);
  if (r.count == 0) {
    return;
  }
  switch (column.type().index()) {
    case StorageType::GetTypeIndex<Id>():
      return Compare<Id>(condition, column, pool, r, cast);
    case StorageType::GetTypeIndex<Uint32>():
      return Compare<Uint32>(condition, column, pool, r, cast);
    case StorageType::GetTypeIndex<Int32>():
      return Compare<Int32>(condition, column, pool, r, cast);
    case StorageType::GetTypeIndex<Int64>():
      return Compare<Int64>(condition, column, pool, r, cast);
    case StorageType::GetTypeIndex<Double>():
      return Compare<Double>(condition, column, pool, r, cast);
    case StorageType::GetTypeIndex<String>():
      return Compare<String>(condition, column, pool, r, cast);
    default:
      PERFETTO_FATAL("Unknown storage type");
  }
}

}  // namespace

Filter::Filter(std::vector<Condition> conditions, const StringPool* pool)
    : conditions_(std::move(conditions)), pool_(pool) {}

Filter::~Filter() = default;
Filter::State::~State() = default;

std::unique_ptr<OperatorState> Filter::MakeState() const {
  auto state = std::make_unique<State>();
  state->lists.resize(conditions_.size());
  return state;
}

OpResult Filter::Execute(const RowBatch& in,
                         RowBatch& out,
                         OperatorState& state) const {
  State& s = state.Cast<State>();
  Rows r{s.rows.data(), s.storage.data(), in.size()};
  for (uint32_t i = 0; i < r.count; ++i) {
    s.rows[i] = i;
  }
  for (uint32_t c = 0; c < conditions_.size(); ++c) {
    const Condition& condition = conditions_[c];
    const ColumnView& column = in.column(condition.column);
    for (uint32_t i = 0; i < r.count; ++i) {
      r.storage[i] = column.selection().GetIndex(r.rows[i]);
    }
    Apply(condition, column, *pool_, r, s.lists[c]);
    if (r.count == 0) {
      break;
    }
  }
  out.CopyFrom(in);
  out.Slice(RowSelection::Indices(
                Span<const uint32_t>(s.rows.data(), s.rows.data() + r.count)),
            r.count);
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
