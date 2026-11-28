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

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/util/glob.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueListResult;
using filter::CastFilterValueResult;

// The batch rows kept so far: `count` of them from `rows`, in the order they
// are kept. Each condition narrows them in place.
struct Rows {
  uint32_t* rows;
  uint32_t count;
  // Whether the rows are 0 to count - 1, not yet written to `rows`.
  bool contiguous;
};

using CompareFn = Rows (*)(const ColumnView&,
                           const ParamValue&,
                           const StringPool&,
                           Rows);

// Writes contiguous rows out, for the steps which read them one by one.
PERFETTO_NO_INLINE Rows Materialize(Rows r) {
  std::iota(r.rows, r.rows + r.count, 0u);
  r.contiguous = false;
  return r;
}

// Keeps the rows which are null if `nulls`, or which are not otherwise.
PERFETTO_NO_INLINE Rows KeepNulls(const ColumnView& column,
                                  bool nulls,
                                  Rows r) {
  const BitVector* validity = column.validity();
  if (column.kind() != ColumnView::Kind::kVariant && !validity) {
    r.count = nulls ? 0 : r.count;
    return r;
  }
  if (r.contiguous) {
    r = Materialize(r);
  }
  if (column.kind() == ColumnView::Kind::kVariant) {
    const auto* data = static_cast<const Variant*>(column.data());
    uint32_t start = column.start();
    uint32_t* rows = r.rows;
    uint32_t count = r.count;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t row = rows[i];
      if ((data[start + row].type == Variant::Type::kNull) == nulls) {
        rows[kept++] = row;
      }
    }
    r.count = kept;
    return r;
  }
  uint32_t start = column.start();
  uint32_t* rows = r.rows;
  uint32_t count = r.count;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t row = rows[i];
    if (validity->is_set(start + row) != nulls) {
      rows[kept++] = row;
    }
  }
  r.count = kept;
  return r;
}

// Keeps the rows of a flat `column` with a validity bitvector which are not
// null. Inline, as every lookup on a nullable key column runs it.
PERFETTO_ALWAYS_INLINE inline Rows KeepNonNullFlat(const ColumnView& column,
                                                   Rows r) {
  if (r.contiguous) {
    r = Materialize(r);
  }
  const BitVector* validity = column.validity();
  uint32_t start = column.start();
  uint32_t* rows = r.rows;
  uint32_t count = r.count;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t row = rows[i];
    if (validity->is_set(start + row)) {
      rows[kept++] = row;
    }
  }
  r.count = kept;
  return r;
}

PERFETTO_NO_INLINE Rows KeepGlobMatches(const ColumnView& column,
                                        GlobMatch& glob,
                                        const StringPool& pool,
                                        Rows r);

// Keeps the rows of a column of storage type T which compare true by O with
// the param. The kernels read each row's value at the column's data
// plus the row, so the data passed starts where the window does.
template <typename T, typename O>
Rows Compare(const ColumnView& column,
             const ParamValue& cast,
             const StringPool& pool,
             Rows r) {
  // The value was cast for the column's storage type, which picks its
  // alternative. A string compared by = or != is cast to its id instead.
  if constexpr (!std::is_same_v<T, String> ||
                (!std::is_same_v<O, Eq> && !std::is_same_v<O, Ne>)) {
    PERFETTO_DCHECK(cast.value.value.index() == StorageType::GetTypeIndex<T>());
  }
  if (r.contiguous) {
    // The rows are a run, so the values are read in order with no list.
    if constexpr (std::is_same_v<T, String>) {
      if constexpr (std::is_same_v<O, Eq> || std::is_same_v<O, Ne>) {
        const auto* data =
            static_cast<const uint32_t*>(column.data()) + column.start();
        using Cmp = std::conditional_t<std::is_same_v<O, Eq>, std::equal_to<>,
                                       std::not_equal_to<>>;
        r.count = static_cast<uint32_t>(
            filter::FilterRange(data, 0, r.count, r.rows,
                                cast.string_id.raw_id(), Cmp()) -
            r.rows);
        r.contiguous = false;
        return r;
      }
    } else {
      using M =
          StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
      const auto* data = static_cast<const M*>(column.data()) + column.start();
      // A local, not to be reloaded after every row written.
      M value = base::unchecked_get<M>(cast.value.value);
      r.count = static_cast<uint32_t>(
          filter::FilterRange(
              data, 0, r.count, r.rows, value,
              filter::comparators::IntegerOrDoubleComparator<M, O>()) -
          r.rows);
      r.contiguous = false;
      return r;
    }
  }
  if (r.contiguous) {
    r = Materialize(r);
  }
  uint32_t* begin = r.rows;
  uint32_t* end = r.rows + r.count;
  uint32_t* kept;
  if constexpr (std::is_same_v<T, String>) {
    if constexpr (std::is_same_v<O, Glob>) {
      if (GlobMatch* glob = cast.glob.get()) {
        return KeepGlobMatches(column, *glob, pool, r);
      }
    }
    const auto* data =
        static_cast<const StringPool::Id*>(column.data()) + column.start();
    if constexpr (std::is_same_v<O, Eq> || std::is_same_v<O, Ne>) {
      // Equal strings are the same string in the pool.
      static_assert(sizeof(StringPool::Id) == sizeof(uint32_t));
      using Cmp = std::conditional_t<std::is_same_v<O, Eq>, std::equal_to<>,
                                     std::not_equal_to<>>;
      kept = filter::Filter(reinterpret_cast<const uint32_t*>(data), begin, end,
                            begin, cast.string_id.raw_id(), Cmp());
    } else if constexpr (std::is_same_v<O, Regex>) {
      kept = filter::StringFilterRegex(&pool, data, *cast.regex, begin, end,
                                       begin);
    } else {
      kept = filter::FilterStringCompare<O>(&pool, data, begin, end, begin,
                                            cast.text.c_str());
    }
  } else {
    using M = StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    const auto* data = static_cast<const M*>(column.data()) + column.start();
    M value = base::unchecked_get<M>(cast.value.value);
    kept =
        filter::Filter(data, begin, end, begin, value,
                       filter::comparators::IntegerOrDoubleComparator<M, O>());
  }
  r.count = static_cast<uint32_t>(kept - begin);
  return r;
}

// Keeps the rows of a column of storage type T whose value is in the param's
// IN list.
template <typename T>
Rows KeepIn(const ColumnView& column,
            const ParamValue& cast,
            const StringPool&,
            Rows r) {
  using M =
      StorageType::VariantTypeAtIndex<T, CastFilterValueListResult::Value>;
  const auto* data = static_cast<const M*>(column.data()) + column.start();
  if (r.contiguous) {
    r = Materialize(r);
  }
  base::Span<uint32_t> rows(r.rows, r.rows + r.count);
  base::Span<uint32_t> dest = rows;
  filter::NonIndexedFilterInScan<T>(*cast.list, data, column.start(), rows,
                                    dest);
  r.count = static_cast<uint32_t>(dest.size());
  return r;
}

using CompareTable =
    std::array<std::array<CompareFn, StorageType::kSize>, Op::kSize>;

template <typename T>
constexpr void FillCompareColumn(CompareTable& t) {
  constexpr size_t kCol = StorageType::GetTypeIndex<T>();
  t[Op::GetTypeIndex<Eq>()][kCol] = &Compare<T, Eq>;
  t[Op::GetTypeIndex<Ne>()][kCol] = &Compare<T, Ne>;
  t[Op::GetTypeIndex<Lt>()][kCol] = &Compare<T, Lt>;
  t[Op::GetTypeIndex<Le>()][kCol] = &Compare<T, Le>;
  t[Op::GetTypeIndex<Gt>()][kCol] = &Compare<T, Gt>;
  t[Op::GetTypeIndex<Ge>()][kCol] = &Compare<T, Ge>;
  t[Op::GetTypeIndex<In>()][kCol] = &KeepIn<T>;
  if constexpr (std::is_same_v<T, String>) {
    t[Op::GetTypeIndex<Glob>()][kCol] = &Compare<T, Glob>;
    t[Op::GetTypeIndex<Regex>()][kCol] = &Compare<T, Regex>;
  }
}

constexpr CompareTable MakeCompareTable() {
  CompareTable t{};
  FillCompareColumn<Uint32>(t);
  FillCompareColumn<Int32>(t);
  FillCompareColumn<Int64>(t);
  FillCompareColumn<Double>(t);
  FillCompareColumn<String>(t);
  return t;
}

constexpr CompareTable kCompareTable = MakeCompareTable();

// Keeps the rows of a string `column` whose value `glob`'s pattern matches.
// Rows are matched one at a time until as many have been as a quarter of the
// pool's strings; then, as the interpreter does for a large input, every
// string in the pool is matched in one pass, and each row after is a bit test.
PERFETTO_NO_INLINE Rows KeepGlobMatches(const ColumnView& column,
                                        GlobMatch& glob,
                                        const StringPool& pool,
                                        Rows r) {
  const util::GlobMatcher& matcher = glob.matcher;
  if (r.contiguous) {
    r = Materialize(r);
  }
  const auto* data =
      static_cast<const StringPool::Id*>(column.data()) + column.start();
  uint32_t kept = 0;
  if (!glob.matched_below && glob.rows >= pool.size() / 4) {
    uint32_t ids = pool.MaxSmallStringId().raw_id() + 1;
    glob.matches = BitVector::CreateWithSize(ids);
    for (auto it = pool.CreateSmallStringIterator(); it; ++it) {
      uint32_t raw = it.StringId().raw_id();
      if (raw >= ids) {
        break;
      }
      if (matcher.Matches(it.StringView())) {
        glob.matches.set(raw);
      }
      glob.matched_below = raw + 1;
    }
  }
  uint32_t* rows = r.rows;
  uint32_t count = r.count;
  uint32_t matched_below = glob.matched_below;
  if (!matched_below) {
    glob.rows += count;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t row = rows[i];
      rows[kept] = row;
      kept += matcher.Matches(pool.Get(data[row]));
    }
    r.count = kept;
    return r;
  }
  const BitVector& matches = glob.matches;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t row = rows[i];
    StringPool::Id id = data[row];
    uint32_t raw = id.raw_id();
    bool match = id.is_large_string() || raw >= matched_below
                     ? matcher.Matches(pool.Get(id))
                     : matches.is_set(raw);
    rows[kept] = row;
    kept += match;
  }
  r.count = kept;
  return r;
}

// Copies the values of a sparse `column` at the rows kept, none of which is
// null, into `values` at each row, and views them as a flat column.
template <uint32_t kWidth>
PERFETTO_NO_INLINE std::pair<ColumnView, Rows>
GatherSparse(const ColumnView& column, Rows r, uint8_t* values) {
  if (r.contiguous) {
    r = Materialize(r);
  }
  const auto* packed = static_cast<const uint8_t*>(column.sparse_data());
  // Locals: the bytes written could otherwise alias any of them.
  const uint32_t* rows = r.rows;
  uint32_t count = r.count;
  uint32_t start = column.start();
  const BitVector* validity = column.validity();
  const uint32_t* popcount = column.popcount();
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t row = rows[i];
    uint32_t slot = filter::IndexToStorageIndex<SparseNull>(start + row,
                                                            validity, popcount);
    uint8_t* value = values + uint64_t{row} * kWidth;
    if (packed) {
      memcpy(value, packed + uint64_t{slot} * kWidth, kWidth);
    } else {
      // A sparse column with no storage holds its slots.
      memcpy(value, &slot, sizeof(slot));
    }
  }
  return {ColumnView::Reference(column.type(), values), r};
}

// Writes the values of a sequence at the rows kept into `values` at each row,
// and views them as a flat column. Contiguous rows stay contiguous, so the
// comparison can still filter them as a range.
PERFETTO_NO_INLINE ColumnView GatherSequence(const ColumnView& column,
                                             Rows r,
                                             uint32_t* values) {
  uint32_t start = column.start();
  if (r.contiguous) {
    std::iota(values, values + r.count, start);
  } else {
    const uint32_t* rows = r.rows;
    uint32_t count = r.count;
    for (uint32_t i = 0; i < count; ++i) {
      values[rows[i]] = start + rows[i];
    }
  }
  return ColumnView::Reference(StorageType{Uint32{}}, values);
}

PERFETTO_NO_INLINE void UnpackSelection(const uint16_t* indices,
                                        uint32_t count,
                                        uint32_t* out) {
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = indices[i];
  }
}

// Applies a comparison or IN to the rows kept so far where Process's fast path
// cannot, dropping the nulls first. The values of a sparse column or a
// sequence at those rows are written to `sparse_values` first.
PERFETTO_NO_INLINE Rows ApplySlow(const ColumnView& lent,
                                  Op op,
                                  const ParamValue& cast,
                                  const StringPool& pool,
                                  uint64_t* sparse_values,
                                  Rows r) {
  if (cast.value.validity == CastFilterValueResult::kNoneMatch) {
    r.count = 0;
    return r;
  }
  if (lent.kind() == ColumnView::Kind::kVariant || lent.validity() != nullptr) {
    r = KeepNulls(lent, /*nulls=*/false, r);
  }
  if (cast.value.validity != CastFilterValueResult::kValid || r.count == 0) {
    return r;
  }
  ColumnView column = lent;
  if (lent.kind() == ColumnView::Kind::kSparse) {
    auto* values = reinterpret_cast<uint8_t*>(sparse_values);
    auto gathered = ValueWidth(lent.type()) == sizeof(uint32_t)
                        ? GatherSparse<sizeof(uint32_t)>(lent, r, values)
                        : GatherSparse<sizeof(uint64_t)>(lent, r, values);
    column = gathered.first;
    r = gathered.second;
  } else if (lent.kind() == ColumnView::Kind::kSequence) {
    column =
        GatherSequence(lent, r, reinterpret_cast<uint32_t*>(sparse_values));
  }
  CompareFn fn = kCompareTable[op.index()][column.type().index()];
  PERFETTO_DCHECK(fn != nullptr);
  return fn(column, cast, pool, r);
}

}  // namespace

Filter::Filter(std::vector<Condition> conditions, const StringPool* pool)
    : conditions_(std::move(conditions)), pool_(pool) {
  for (const Condition& c : conditions_) {
    PERFETTO_CHECK(c.op.IsAnyOf<NonStringOp>() || c.op.IsAnyOf<NullOp>() ||
                   c.op.Is<In>() || c.op.IsAnyOf<OnlyStringOp>());
  }
}

Filter::~Filter() = default;
Filter::State::~State() = default;

std::unique_ptr<OperatorState> Filter::MakeState(Context& context) const {
  return std::make_unique<State>(context);
}

bool Filter::Process(RowBatch& batch, OperatorState& state) const {
  State& s = state.Cast<State>();
  const Selection& selection = batch.selection();
  Rows r{s.rows.data(), selection.size(), selection.prefix()};
  if (PERFETTO_UNLIKELY(r.count == 0 || conditions_.empty())) {
    return true;
  }
  if (PERFETTO_UNLIKELY(!r.contiguous)) {
    UnpackSelection(selection.indices(), r.count, r.rows);
  }
  const ParamValue* values = s.context->params().b;
  for (const Condition& c : conditions_) {
    const ColumnView& column = batch.column(c.column);
    if (PERFETTO_UNLIKELY(c.op.IsAnyOf<NullOp>())) {
      r = KeepNulls(column, c.op.Is<IsNull>(), r);
    } else {
      PERFETTO_DCHECK(column.kind() != ColumnView::Kind::kVariant);
      PERFETTO_DCHECK(c.param < s.context->params().size());
      const ParamValue& cast = values[c.param];
      // A dense nullable flat column's null slots hold values, so a number
      // or a string id is compared on every row, and the nulls are dropped
      // only from the rows left. Other string comparisons read the pool,
      // which a null's slot need not be in.
      bool nulls_first = column.validity() != nullptr &&
                         column.type().Is<String>() && !c.op.Is<Eq>() &&
                         !c.op.Is<Ne>();
      if (PERFETTO_UNLIKELY(
              cast.value.validity != CastFilterValueResult::kValid ||
              column.kind() != ColumnView::Kind::kFlat || nulls_first)) {
        r = ApplySlow(column, c.op, cast, *pool_, s.sparse_values.data(), r);
      } else {
        CompareFn fn = kCompareTable[c.op.index()][column.type().index()];
        PERFETTO_DCHECK(fn != nullptr);
        r = fn(column, cast, *pool_, r);
        if (column.validity() != nullptr && r.count != 0) {
          r = KeepNonNullFlat(column, r);
        }
      }
    }
    if (r.count == 0) {
      break;
    }
  }
  if (PERFETTO_UNLIKELY(r.contiguous)) {
    if (r.count == selection.size()) {
      return true;
    }
    r = Materialize(r);
  }
  batch.mutable_selection().KeepRows(
      base::Span<const uint32_t>(r.rows, r.rows + r.count));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
