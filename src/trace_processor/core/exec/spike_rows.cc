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

#include "src/trace_processor/core/exec/spike_rows.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueResult;

}  // namespace

ColumnView ViewOfColumn(const dataframe::Column& column,
                        std::vector<Slab<uint32_t>>* prefixes) {
  StorageType type = column.storage.type();
  const auto& nulls = column.null_storage;
  const BitVector* bits = nulls.MaybeGetNullBitVector();
  if (type.Is<Id>()) {
    if (nulls.nullability().Is<SparseNull>() ||
        nulls.nullability().Is<SparseNullWithPopcountAlways>() ||
        nulls.nullability().Is<SparseNullWithPopcountUntilFinalization>()) {
      PERFETTO_FATAL("SPIKE: sparse id column");
    }
    return ColumnView::Reference(type, nullptr, bits);
  }
  const void* data = nullptr;
  switch (type.index()) {
    case StorageType::GetTypeIndex<Uint32>():
      data = column.storage.unchecked_data<Uint32>();
      break;
    case StorageType::GetTypeIndex<Int32>():
      data = column.storage.unchecked_data<Int32>();
      break;
    case StorageType::GetTypeIndex<Int64>():
      data = column.storage.unchecked_data<Int64>();
      break;
    case StorageType::GetTypeIndex<Double>():
      data = column.storage.unchecked_data<Double>();
      break;
    case StorageType::GetTypeIndex<String>():
      data = column.storage.unchecked_data<String>();
      break;
    default:
      PERFETTO_FATAL("Unknown storage type");
  }
  if (!bits || nulls.nullability().Is<DenseNull>()) {
    return ColumnView::Reference(type, data, bits);
  }
  prefixes->push_back(bits->PrefixPopcount());
  return ColumnView::Sparse(type, data, bits, prefixes->back().data());
}

namespace {

template <typename T>
std::optional<T> ValueAt(const ColumnView& view, uint32_t row) {
  if (view.validity() && !view.validity()->is_set(row)) {
    return std::nullopt;
  }
  if (view.kind() == ColumnView::Kind::kSequence) {
    return static_cast<T>(row);
  }
  return static_cast<const T*>(view.data())[view.StorageIndex(row)];
}

template <typename T>
Rows NarrowTyped(const ColumnView& view, Op op, T value, Rows rows) {
  auto row_at = [&](uint32_t at) {
    return rows.permutation ? rows.permutation[at] : at;
  };
  // Positions are [nulls, values...], each part ordered.
  uint32_t lo = rows.begin;
  uint32_t hi = rows.end;
  auto partition = [&](auto pred) {
    uint32_t first = lo;
    uint32_t count = hi - lo;
    while (count > 0) {
      uint32_t step = count / 2;
      uint32_t mid = first + step;
      if (pred(row_at(mid))) {
        first = mid + 1;
        count -= step + 1;
      } else {
        count = step;
      }
    }
    return first;
  };
  // A column with no nulls has none to skip.
  uint32_t values =
      view.validity()
          ? partition([&](uint32_t r) { return !ValueAt<T>(view, r); })
          : lo;
  lo = values;
  uint32_t lower;
  uint32_t upper;
  if constexpr (std::is_integral_v<T>) {
    if (view.kind() == ColumnView::Kind::kSequence && !rows.permutation &&
        !view.validity()) {
      // In the table's own order a sequence's value is its position, so
      // where a value sits is arithmetic rather than a search.
      auto clamp = [&](int64_t at) {
        return static_cast<uint32_t>(
            std::clamp<int64_t>(at, int64_t{lo}, int64_t{hi}));
      };
      lower = clamp(int64_t{value});
      upper = clamp(int64_t{value} + 1);
    } else {
      lower =
          partition([&](uint32_t r) { return *ValueAt<T>(view, r) < value; });
      upper = partition(
          [&](uint32_t r) { return !(value < *ValueAt<T>(view, r)); });
    }
  } else {
    lower = partition([&](uint32_t r) { return *ValueAt<T>(view, r) < value; });
    upper =
        partition([&](uint32_t r) { return !(value < *ValueAt<T>(view, r)); });
  }
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return {rows.permutation, lower, upper};
    case Op::GetTypeIndex<Lt>():
      return {rows.permutation, values, lower};
    case Op::GetTypeIndex<Le>():
      return {rows.permutation, values, upper};
    case Op::GetTypeIndex<Gt>():
      return {rows.permutation, upper, hi};
    case Op::GetTypeIndex<Ge>():
      return {rows.permutation, lower, hi};
    default:
      PERFETTO_FATAL("Unsupported narrowing");
  }
}

// Keeps the rows holding a value, or none.
uint32_t* KeepNulls(const ColumnView& view,
                    uint32_t count,
                    bool keep_null,
                    uint32_t* w) {
  RowSelection selection = view.selection();
  const BitVector* validity = view.validity();
  bool strings =
      view.type().Is<String>() && view.kind() != ColumnView::Kind::kSequence;
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = selection.GetIndex(row);
    bool null = validity && !validity->is_set(index);
    if (!null && strings) {
      null = static_cast<const StringPool::Id*>(
                 view.data())[view.StorageIndex(index)]
                 .is_null();
    }
    if (null == keep_null) {
      *w++ = row;
    }
  }
  return w;
}

}  // namespace

Rows Narrow(const ColumnView& view,
            Op op,
            const CastFilterValueResult& value,
            Rows rows) {
  if (value.validity == CastFilterValueResult::kNoneMatch) {
    rows.end = rows.begin;
    return rows;
  }
  if (value.validity == CastFilterValueResult::kAllMatch) {
    // Every value matches, but no null does: they sort first.
    const BitVector* validity = view.validity();
    if (validity) {
      uint32_t first = rows.begin;
      uint32_t count = rows.end - rows.begin;
      while (count > 0) {
        uint32_t step = count / 2;
        uint32_t mid = first + step;
        uint32_t row = rows.permutation ? rows.permutation[mid] : mid;
        if (!validity->is_set(row)) {
          first = mid + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }
      rows.begin = first;
    }
    return rows;
  }
  const auto& v = value.value;
  switch (v.index()) {
    case 0:
      return NarrowTyped<uint32_t>(
          view, op, base::unchecked_get<CastFilterValueResult::Id>(v).value, rows);
    case 1:
      return NarrowTyped<uint32_t>(view, op, base::unchecked_get<uint32_t>(v), rows);
    case 2:
      return NarrowTyped<int32_t>(view, op, base::unchecked_get<int32_t>(v), rows);
    case 3:
      return NarrowTyped<int64_t>(view, op, base::unchecked_get<int64_t>(v), rows);
    case 4:
      return NarrowTyped<double>(view, op, base::unchecked_get<double>(v), rows);
    default:
      PERFETTO_FATAL("Unsupported narrowing");
  }
}

namespace {

template <typename T, typename V>
Rows NarrowAs(const ColumnView& view,
              Op op,
              const CastFilterValueResult& value,
              Rows rows) {
  if (value.validity != CastFilterValueResult::kValid) {
    return Narrow(view, op, value, rows);
  }
  T key;
  if constexpr (std::is_same_v<V, CastFilterValueResult::Id>) {
    key = base::unchecked_get<V>(value.value).value;
  } else {
    key = base::unchecked_get<V>(value.value);
  }
  return NarrowTyped<T>(view, op, key, rows);
}

}  // namespace

namespace {

// Narrowing over the table's own order of a column with no nulls: its rows
// are its values' positions for a sequence, else a binary search over them.
template <typename T, typename V, typename O, bool kSequence>
Rows NarrowOwnOrder(const ColumnView& view,
                    Op op,
                    const CastFilterValueResult& value,
                    Rows rows) {
  if (value.validity != CastFilterValueResult::kValid) {
    return Narrow(view, op, value, rows);
  }
  T key;
  if constexpr (std::is_same_v<V, CastFilterValueResult::Id>) {
    key = base::unchecked_get<V>(value.value).value;
  } else {
    key = base::unchecked_get<V>(value.value);
  }
  uint32_t lo = rows.begin;
  uint32_t hi = rows.end;
  uint32_t lower;
  uint32_t upper;
  // Whether the column is a sequence is known when the plan is built.
  if constexpr (kSequence) {
    // In the table's own order a sequence's value is its position.
    static_assert(std::is_integral_v<T>);
    auto clamp = [&](int64_t at) {
      return static_cast<uint32_t>(
          std::clamp<int64_t>(at, int64_t{lo}, int64_t{hi}));
    };
    lower = clamp(int64_t{key});
    upper = clamp(int64_t{key} + 1);
  } else {
    const T* data = static_cast<const T*>(view.data());
    lower = static_cast<uint32_t>(std::lower_bound(data + lo, data + hi, key) -
                                  data);
    upper =
        std::is_same_v<O, Eq> || std::is_same_v<O, Le> || std::is_same_v<O, Gt>
            ? static_cast<uint32_t>(
                  std::upper_bound(data + lower, data + hi, key) - data)
            : lower;
  }
  if constexpr (std::is_same_v<O, Eq>) {
    return {nullptr, lower, upper};
  } else if constexpr (std::is_same_v<O, Lt>) {
    return {nullptr, lo, lower};
  } else if constexpr (std::is_same_v<O, Le>) {
    return {nullptr, lo, upper};
  } else if constexpr (std::is_same_v<O, Gt>) {
    return {nullptr, upper, hi};
  } else {
    static_assert(std::is_same_v<O, Ge>);
    return {nullptr, lower, hi};
  }
}

template <typename T, typename V, bool kSequence = false>
Narrower OwnOrderFor(Op op) {
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return &NarrowOwnOrder<T, V, Eq, kSequence>;
    case Op::GetTypeIndex<Lt>():
      return &NarrowOwnOrder<T, V, Lt, kSequence>;
    case Op::GetTypeIndex<Le>():
      return &NarrowOwnOrder<T, V, Le, kSequence>;
    case Op::GetTypeIndex<Gt>():
      return &NarrowOwnOrder<T, V, Gt, kSequence>;
    case Op::GetTypeIndex<Ge>():
      return &NarrowOwnOrder<T, V, Ge, kSequence>;
    default:
      return &Narrow;
  }
}

}  // namespace

Narrower NarrowerFor(const ColumnView& column, Op op, bool permuted) {
  StorageType type = column.type();
  if (!permuted && !column.validity()) {
    switch (type.index()) {
      case StorageType::GetTypeIndex<Id>():
        return column.kind() == ColumnView::Kind::kSequence
                   ? OwnOrderFor<uint32_t, CastFilterValueResult::Id, true>(op)
                   : OwnOrderFor<uint32_t, CastFilterValueResult::Id>(op);
      case StorageType::GetTypeIndex<Uint32>():
        return OwnOrderFor<uint32_t, uint32_t>(op);
      case StorageType::GetTypeIndex<Int32>():
        return OwnOrderFor<int32_t, int32_t>(op);
      case StorageType::GetTypeIndex<Int64>():
        return OwnOrderFor<int64_t, int64_t>(op);
      case StorageType::GetTypeIndex<Double>():
        return OwnOrderFor<double, double>(op);
      default:
        break;
    }
  }
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
      return &NarrowAs<uint32_t, CastFilterValueResult::Id>;
    case StorageType::GetTypeIndex<Uint32>():
      return &NarrowAs<uint32_t, uint32_t>;
    case StorageType::GetTypeIndex<Int32>():
      return &NarrowAs<int32_t, int32_t>;
    case StorageType::GetTypeIndex<Int64>():
      return &NarrowAs<int64_t, int64_t>;
    case StorageType::GetTypeIndex<Double>():
      return &NarrowAs<double, double>;
    default:
      return &Narrow;
  }
}

RowsScan::RowsScan(const dataframe::Dataframe& df,
                   std::vector<uint32_t> columns,
                   const uint32_t* permutation,
                   std::vector<Narrowing> narrowings,
                   bool empty)
    : permutation_(permutation),
      // A plan known to be empty is one over no rows.
      row_count_(empty ? 0 : df.row_count()),
      column_count_(static_cast<uint32_t>(columns.size())) {
  for (Narrowing& n : narrowings) {
    narrowings_.emplace_back(std::move(n));
  }
  prefixes_.reserve(columns.size());
  for (uint32_t c : columns) {
    views_.push_back(ViewOfColumn(df.column(c), &prefixes_));
  }
}
RowsScan::~RowsScan() = default;

std::unique_ptr<OperatorState> RowsScan::MakeState() const {
  return std::make_unique<State>();
}

void RowsScan::Rewind(OperatorState& state) const {
  state.Cast<State>().started = false;
}

// The rows this run reads, narrowed by the values cast for it.
PERFETTO_ALWAYS_INLINE Rows RowsScan::Narrowed() const {
  Rows rows{permutation_, 0, row_count_};
  for (const Narrowing& n : narrowings_) {
    if (rows.end == rows.begin) {
      break;
    }
    rows = n.narrow(n.column, n.op, *n.value, rows);
  }
  return rows;
}

RowBatch* RowsScan::Open(RowBatch&, OperatorState& state) const {
  State& s = state.Cast<State>();
  Rows rows = Narrowed();
  s.started = true;
  return Produce(s, rows.begin, rows.end);
}

RowBatch* RowsScan::Next(RowBatch&, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (!s.started) {
    Rows rows = Narrowed();
    s.started = true;
    return Produce(s, rows.begin, rows.end);
  }
  return Produce(s, s.next, s.end);
}

// Out of line, so the views it copies stay off the hot path's stack.
PERFETTO_NO_INLINE void RowsScan::Settle(RowBatch& out) const {
  if (out.column_count() != column_count_ ||
      (out.changes() & RowBatch::kReplaced)) {
    out.Reset();
    for (const ColumnView& view : views_) {
      out.AddColumn(view);
    }
  } else if (out.changes() & RowBatch::kComposed) {
    // Lets go of selections something composed into the columns.
    for (uint32_t i = 0; i < out.column_count(); ++i) {
      out.mutable_column(i).SetRange(0);
    }
  }
  out.mark_settled();
}

// The batch is this scan's own and laid out once: a run only points its
// columns at their rows, unless something since changed them. Positions are
// kept in locals and stored once: stores to the columns could alias them.
PERFETTO_ALWAYS_INLINE RowBatch* RowsScan::Produce(State& s,
                                                   uint32_t next,
                                                   uint32_t end) const {
  if (next >= end) {
    s.next = next;
    s.end = end;
    return nullptr;
  }
  uint32_t count = std::min(kMaxBatchRows, end - next);
  RowBatch& out = s.batch;
  if (PERFETTO_UNLIKELY(out.changes())) {
    Settle(out);
  }
  RowSelection selection =
      permutation_ ? RowSelection::Indices(Span<const uint32_t>(
                         permutation_ + next, permutation_ + next + count))
                   : RowSelection::Range(next);
  uint32_t columns = column_count_;
  for (uint32_t i = 0; i < columns; ++i) {
    out.mutable_column(i).PointAt(selection);
  }
  next += count;
  s.next = next;
  s.end = end;
  out.SetCardinality(count);
  out.set_last(next >= end);
  return &out;
}

bool RowsScan::GetData(RowBatch& out, OperatorState& state) const {
  RowBatch* batch = Next(out, state);
  if (!batch) {
    return false;
  }
  out.CopyFrom(*batch);
  return true;
}

namespace {

template <typename T, typename O>
uint32_t* NumericKernel(const void* data,
                        const uint32_t* begin,
                        const uint32_t* end,
                        uint32_t* out,
                        const CastFilterValueResult& value,
                        const StringPool*) {
  using M = StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
  const M& v = base::unchecked_get<M>(value.value);
  if constexpr (std::is_same_v<T, Id>) {
    base::ignore_result(data);
    return filter::IdentityFilter(
        begin, end, out, v.value,
        filter::comparators::IntegerOrDoubleComparator<uint32_t, O>());
  } else {
    return filter::Filter(
        static_cast<const M*>(data), begin, end, out, v,
        filter::comparators::IntegerOrDoubleComparator<M, O>());
  }
}

template <typename O>
uint32_t* StringKernel(const void* data,
                       const uint32_t* begin,
                       const uint32_t* end,
                       uint32_t* out,
                       const CastFilterValueResult& value,
                       const StringPool* pool) {
  return filter::FilterStringCompare<O>(
      pool, static_cast<const StringPool::Id*>(data), begin, end, out,
      base::unchecked_get<const char*>(value.value));
}

template <typename T>
uint32_t* LinearEq(const void* data,
                   uint32_t count,
                   uint32_t* out,
                   const CastFilterValueResult& value,
                   const StringPool* pool) {
  using D = std::conditional_t<
      std::is_same_v<T, String>, uint32_t,
      StorageType::VariantTypeAtIndex<T, CastFilterValueResult::Value>>;
  D v;
  if constexpr (std::is_same_v<T, String>) {
    std::optional<StringPool::Id> id = pool->GetId(
        base::StringView(base::unchecked_get<const char*>(value.value)));
    if (!id) {
      return out;
    }
    v = id->raw_id();
  } else {
    base::ignore_result(pool);
    v = base::unchecked_get<D>(value.value);
  }
  const D* d = static_cast<const D*>(data);
  for (uint32_t row = 0; row < count; ++row) {
    // Branchy on purpose, as the interpreter's: few rows match.
    if (d[row] == v) {
      *out++ = row;
    }
  }
  return out;
}

template <typename T>
auto NumericKernelFor(Op op) {
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return &NumericKernel<T, Eq>;
    case Op::GetTypeIndex<Ne>():
      return &NumericKernel<T, Ne>;
    case Op::GetTypeIndex<Lt>():
      return &NumericKernel<T, Lt>;
    case Op::GetTypeIndex<Le>():
      return &NumericKernel<T, Le>;
    case Op::GetTypeIndex<Gt>():
      return &NumericKernel<T, Gt>;
    case Op::GetTypeIndex<Ge>():
      return &NumericKernel<T, Ge>;
    default:
      PERFETTO_FATAL("Unsupported op");
  }
}

}  // namespace

ColumnFilter::ColumnFilter(uint32_t column,
                           StorageType type,
                           Op op,
                           const CastFilterValueResult* value,
                           const StringPool* pool)
    : column_(column), value_(value), pool_(pool) {
  if (op.Is<IsNull>() || op.Is<IsNotNull>()) {
    keep_null_ = op.Is<IsNull>();
    return;
  }
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
      kernel_ = NumericKernelFor<Id>(op);
      break;
    case StorageType::GetTypeIndex<Uint32>():
      kernel_ = NumericKernelFor<Uint32>(op);
      linear_ = op.Is<Eq>() ? &LinearEq<Uint32> : nullptr;
      width_ = 4;
      break;
    case StorageType::GetTypeIndex<Int32>():
      kernel_ = NumericKernelFor<Int32>(op);
      linear_ = op.Is<Eq>() ? &LinearEq<Int32> : nullptr;
      width_ = 4;
      break;
    case StorageType::GetTypeIndex<Int64>():
      kernel_ = NumericKernelFor<Int64>(op);
      linear_ = op.Is<Eq>() ? &LinearEq<Int64> : nullptr;
      width_ = 8;
      break;
    case StorageType::GetTypeIndex<Double>():
      kernel_ = NumericKernelFor<Double>(op);
      linear_ = op.Is<Eq>() ? &LinearEq<Double> : nullptr;
      width_ = 8;
      break;
    default:
      kernel_ = op.Is<Eq>() ? &StringKernel<Eq> : &StringKernel<Ne>;
      linear_ = op.Is<Eq>() ? &LinearEq<String> : nullptr;
      drop_null_ids_ = op.Is<Ne>();
      width_ = 4;
      break;
  }
}

bool ColumnFilter::Supports(StorageType type, Op op) {
  switch (op.index()) {
    case Op::GetTypeIndex<IsNull>():
    case Op::GetTypeIndex<IsNotNull>():
      return true;
    case Op::GetTypeIndex<Eq>():
    case Op::GetTypeIndex<Ne>():
      return true;
    case Op::GetTypeIndex<Lt>():
    case Op::GetTypeIndex<Le>():
    case Op::GetTypeIndex<Gt>():
    case Op::GetTypeIndex<Ge>():
      return !type.Is<String>();
    default:
      return false;
  }
}

std::unique_ptr<OperatorState> ColumnFilter::MakeState() const {
  auto state = std::make_unique<State>();
  state->rows.resize(kMaxBatchRows);
  state->indices.resize(kMaxBatchRows);
  return state;
}

// The same for every type and comparison: which rows to compare, and where
// their values are, with the kernel only comparing.
uint32_t* ColumnFilter::Keep(const ColumnView& view,
                             uint32_t count,
                             State& s) const {
  uint32_t* rows = s.rows.data();
  RowSelection selection = view.selection();
  const BitVector* validity = view.validity();
  // A run of rows over values stored densely, null or not: compare them all,
  // then drop the few matches which are nulls, whose values mean nothing.
  if (linear_ && selection.is_range() &&
      view.kind() != ColumnView::Kind::kSparse) {
    uint32_t offset = selection.offset();
    const auto* run =
        static_cast<const uint8_t*>(view.data()) + size_t{offset} * width_;
    uint32_t* end = linear_(run, count, rows, *value_, pool_);
    if (!validity) {
      return end;
    }
    uint32_t* w = rows;
    for (uint32_t* it = rows; it != end; ++it) {
      if (validity->is_set(offset + *it)) {
        *w++ = *it;
      }
    }
    return w;
  }
  for (uint32_t row = 0; row < count; ++row) {
    rows[row] = row;
  }
  bool sequence = view.kind() == ColumnView::Kind::kSequence;
  const void* data = view.data();
  if (selection.is_range() && !validity && !drop_null_ids_ && !sequence) {
    // A run of rows with every value present: the rows index the values,
    // from where the run starts.
    data = static_cast<const uint8_t*>(data) +
           size_t{selection.offset()} * width_;
    return kernel_(data, rows, rows + count, rows, *value_, pool_);
  }
  uint32_t* indices = s.indices.data();
  uint32_t kept = 0;
  bool sparse = view.kind() == ColumnView::Kind::kSparse;
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = selection.GetIndex(row);
    if (validity && !validity->is_set(index)) {
      continue;
    }
    uint32_t at = sparse ? view.StorageIndex(index) : index;
    if (drop_null_ids_ &&
        static_cast<const StringPool::Id*>(data)[at].is_null()) {
      continue;
    }
    indices[kept] = at;
    rows[kept++] = row;
  }
  return kernel_(data, indices, indices + kept, rows, *value_, pool_);
}

bool ColumnFilter::Process(RowBatch& batch, OperatorState& state) const {
  return Run(*this, batch, state);
}

bool ColumnFilter::Run(const Transform& self,
                       RowBatch& batch,
                       OperatorState& state) {
  const auto& f = static_cast<const ColumnFilter&>(self);
  State& s = state.Cast<State>();
  uint32_t count = batch.size();
  const ColumnView& view = batch.column(f.column_);
  uint32_t* rows = s.rows.data();
  uint32_t* w;
  if (!f.kernel_) {
    w = KeepNulls(view, count, f.keep_null_, rows);
  } else if (f.value_->validity == CastFilterValueResult::kNoneMatch) {
    w = rows;
  } else if (f.value_->validity == CastFilterValueResult::kAllMatch) {
    w = KeepNulls(view, count, false, rows);
  } else {
    w = f.Keep(view, count, s);
  }
  auto kept = static_cast<uint32_t>(w - rows);
  if (kept == 0) {
    batch.SetCardinality(0);
  } else if (kept != batch.size()) {
    batch.Slice(RowSelection::Indices(Span<const uint32_t>(rows, w)), kept);
  }
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
