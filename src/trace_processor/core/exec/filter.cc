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

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/dispatch.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/transient_column.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// What every filter needs, whatever shape its input is in.
struct Args {
  uint32_t column;
  uint32_t value_index;
  Op op;
  Span<uint32_t> scratch;
};

// Narrows the caller's value to the column's type. Only the filter knows what
// that type is, which is why the narrowing happens here rather than before the
// tree is reached.
template <typename T>
core::ops::CastResult CastValue(ValueFetcher& values,
                                uint32_t index,
                                Op op,
                                ValueOf<T>& out) {
  if constexpr (std::is_same_v<T, String>) {
    base::ignore_result(values, index, op, out);
    PERFETTO_FATAL("String filter values are not supported yet");
  } else {
    return core::ops::CastFilterValueToIntegerOrDouble(
        index, values.GetValueType(index), values,
        *op.TryDowncast<NonStringOp>(), out);
  }
}

// Filters a batch still holding the source's contiguous rows. Selecting
// physical rows directly lets every column of the batch borrow the result.
template <typename T, typename Compare>
class RangeFilter final : public Operator {
 public:
  explicit RangeFilter(const Args& args)
      : column_(args.column),
        value_index_(args.value_index),
        op_(args.op),
        selected_(args.scratch) {}

  bool Open(ValueFetcher& values) override {
    core::ops::CastResult cast =
        CastValue<T>(values, value_index_, op_, value_);
    enabled_ = cast == core::ops::CastResult::kValid;
    return cast != core::ops::CastResult::kNoneMatch;
  }

  OpResult Execute(RowBatch& chunk) override {
    if (!enabled_) {
      return OpResult::kContinue;
    }
    const TransientColumn& column = chunk.column(column_);
    PERFETTO_DCHECK(column.selection().is_range());
    uint32_t base = column.selection().offset();
    uint32_t* end = core::ops::FilterRange(
        static_cast<const ValueOf<T>*>(column.data()) + base, chunk.size(),
        base, selected_.data(), value_, Compare());
    return chunk.AdoptPhysicalRows(Span<const uint32_t>(selected_.data(), end))
               ? OpResult::kContinue
               : OpResult::kDrop;
  }

 private:
  uint32_t column_;
  uint32_t value_index_;
  Op op_;
  Span<uint32_t> selected_;
  bool enabled_ = true;
  ValueOf<T> value_{};
};

// Filters a batch a previous operator narrowed, so rows are reached through its
// index list and the result composes with it.
template <typename T, typename Compare>
class IndexedFilter final : public Operator {
 public:
  explicit IndexedFilter(const Args& args)
      : column_(args.column),
        value_index_(args.value_index),
        op_(args.op),
        selected_(args.scratch) {}

  bool Open(ValueFetcher& values) override {
    core::ops::CastResult cast =
        CastValue<T>(values, value_index_, op_, value_);
    enabled_ = cast == core::ops::CastResult::kValid;
    return cast != core::ops::CastResult::kNoneMatch;
  }

  OpResult Execute(RowBatch& chunk) override {
    if (!enabled_) {
      return OpResult::kContinue;
    }
    const TransientColumn& column = chunk.column(column_);
    const uint32_t* rows = column.selection().data();
    // The rows walked are already physical, so they are also what the batch
    // wants back: the index array and the emitted array are the same one.
    uint32_t* end = core::ops::FilterIndices(
        static_cast<const ValueOf<T>*>(column.data()), rows,
        rows + chunk.size(), rows, selected_.data(), value_, Compare());
    return chunk.AdoptPhysicalRows(Span<const uint32_t>(selected_.data(), end))
               ? OpResult::kContinue
               : OpResult::kDrop;
  }

 private:
  uint32_t column_;
  uint32_t value_index_;
  Op op_;
  Span<uint32_t> selected_;
  bool enabled_ = true;
  ValueOf<T> value_{};
};

// Resolves the runtime op to the comparator the shared kernels take. The
// comparator is passed rather than named so its type is deduced.
template <typename T, template <typename, typename> class Filter, typename Cmp>
std::unique_ptr<Operator> Make(const Args& args, Cmp) {
  return std::make_unique<Filter<T, Cmp>>(args);
}

template <typename T, template <typename, typename> class Filter>
std::unique_ptr<Operator> MakeComparison(const Args& args) {
  using Cpp = ValueOf<T>;
  if (args.op.Is<Eq>()) {
    return Make<T, Filter>(args, ComparatorFor<Cpp, Eq>());
  }
  if (args.op.Is<Ne>()) {
    return Make<T, Filter>(args, ComparatorFor<Cpp, Ne>());
  }
  if constexpr (!std::is_same_v<T, String>) {
    if (args.op.Is<Lt>()) {
      return Make<T, Filter>(args, ComparatorFor<Cpp, Lt>());
    }
    if (args.op.Is<Le>()) {
      return Make<T, Filter>(args, ComparatorFor<Cpp, Le>());
    }
    if (args.op.Is<Gt>()) {
      return Make<T, Filter>(args, ComparatorFor<Cpp, Gt>());
    }
    if (args.op.Is<Ge>()) {
      return Make<T, Filter>(args, ComparatorFor<Cpp, Ge>());
    }
  }
  PERFETTO_FATAL("Unsupported filter");
}

}  // namespace

std::unique_ptr<Operator> MakeFilter(uint32_t column,
                                     StorageType type,
                                     Op op,
                                     uint32_t value_index,
                                     Span<uint32_t> scratch,
                                     bool contiguous_input) {
  Args args{column, value_index, op, scratch};
  return Dispatch(type, [&](auto t) -> std::unique_ptr<Operator> {
    using T = decltype(t);
    return contiguous_input ? MakeComparison<T, RangeFilter>(args)
                            : MakeComparison<T, IndexedFilter>(args);
  });
}

}  // namespace perfetto::trace_processor::core::exec
