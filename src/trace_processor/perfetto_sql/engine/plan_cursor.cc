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

#include "src/trace_processor/perfetto_sql/engine/plan_cursor.h"

#include <sqlite3.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::ColumnView;
using core::exec::RowBatch;
using core::exec::Variant;

// Hands the values SQLite passes for a plan's parameters to the filter casts:
// at an index, the value of the parameter there, or while iterating, the item
// of its IN list. A blob, which no column holds, is taken as a null, which
// nothing equals.
struct ArgvFetcher : core::ValueFetcher {
  using Type = sqlite::Type;
  static constexpr Type kInt64 = sqlite::Type::kInteger;
  static constexpr Type kDouble = sqlite::Type::kFloat;
  static constexpr Type kString = sqlite::Type::kText;
  static constexpr Type kNull = sqlite::Type::kNull;

  int64_t GetInt64Value(uint32_t i) const {
    return sqlite::value::Int64(Value(i));
  }
  double GetDoubleValue(uint32_t i) const {
    return sqlite::value::Double(Value(i));
  }
  const char* GetStringValue(uint32_t i) const {
    return sqlite::value::Text(Value(i));
  }
  Type GetValueType(uint32_t i) const {
    Type type = sqlite::value::Type(Value(i));
    return type == sqlite::Type::kBlob ? kNull : type;
  }
  bool IteratorInit(uint32_t i) {
    return sqlite3_vtab_in_first(params[i], &item) == SQLITE_OK && item;
  }
  bool IteratorNext(uint32_t i) {
    return sqlite3_vtab_in_next(params[i], &item) == SQLITE_OK && item;
  }
  sqlite3_value* Value(uint32_t i) const { return item ? item : params[i]; }

  sqlite3_value** params;
  sqlite3_value* item = nullptr;
};

void ResultString(sqlite3_context* ctx, StringPool* pool, StringPool::Id id) {
  if (id.is_null()) {
    sqlite::result::Null(ctx);
    return;
  }
  // Interned strings live as long as the pool.
  auto str = pool->Get(id);
  sqlite::result::StaticString(ctx, str.c_str(), static_cast<int>(str.size()));
}

void ResultVariant(sqlite3_context* ctx,
                   StringPool* pool,
                   const RowBatch& batch,
                   uint32_t column,
                   uint32_t row) {
  Variant cell = batch.column(column).At<Variant>(row);
  switch (cell.type) {
    case Variant::Type::kNull:
      return sqlite::result::Null(ctx);
    case Variant::Type::kInt64:
      return sqlite::result::Long(ctx, cell.AsInt64());
    case Variant::Type::kDouble:
      return sqlite::result::Double(ctx, cell.AsDouble());
    case Variant::Type::kString:
      return ResultString(ctx, pool, cell.AsString());
  }
  PERFETTO_FATAL("For GCC");
}

template <typename T>
void ResultFlat(sqlite3_context* ctx,
                StringPool* pool,
                const RowBatch& batch,
                uint32_t column,
                uint32_t row) {
  const ColumnView& view = batch.column(column);
  uint32_t index = view.Index(row);
  if (view.validity() && !view.validity()->is_set(index)) {
    return sqlite::result::Null(ctx);
  }
  T value = static_cast<const T*>(view.data())[index];
  if constexpr (std::is_same_v<T, double>) {
    sqlite::result::Double(ctx, value);
  } else if constexpr (std::is_same_v<T, StringPool::Id>) {
    ResultString(ctx, pool, value);
  } else {
    sqlite::result::Long(ctx, value);
  }
}

// An Id column's values are stored, or are where they would be.
void ResultId(sqlite3_context* ctx,
              StringPool*,
              const RowBatch& batch,
              uint32_t column,
              uint32_t row) {
  const ColumnView& view = batch.column(column);
  if (!view.IsValid(row)) {
    return sqlite::result::Null(ctx);
  }
  sqlite::result::Long(ctx, view.At<uint32_t>(row));
}

// How a column laid out as `layout` is read, the same for every batch.
PlanCursor::ResultFn ReaderFor(const core::exec::ColumnLayout& layout) {
  if (layout.variant()) {
    return &ResultVariant;
  }
  switch (layout.type().index()) {
    case core::StorageType::GetTypeIndex<core::Id>():
      return &ResultId;
    case core::StorageType::GetTypeIndex<core::Uint32>():
      return &ResultFlat<uint32_t>;
    case core::StorageType::GetTypeIndex<core::Int32>():
      return &ResultFlat<int32_t>;
    case core::StorageType::GetTypeIndex<core::Int64>():
      return &ResultFlat<int64_t>;
    case core::StorageType::GetTypeIndex<core::Double>():
      return &ResultFlat<double>;
    case core::StorageType::GetTypeIndex<core::String>():
      return &ResultFlat<StringPool::Id>;
    default:
      PERFETTO_FATAL("Unexpected column type");
  }
}

}  // namespace

PlanCursor::PlanCursor(std::unique_ptr<pipeline::PhysicalPlan> plan,
                       StringPool* pool)
    : plan_(std::move(plan)),
      pool_(pool),
      filter_values_(plan_->filter_values()),
      rows_(plan_->source()) {
  for (const pipeline::PhysicalPlan::Param& param : plan_->params()) {
    binders_.push_back(
        {BinderFor(param), &param, &filter_values_[param.value]});
  }
}

PlanCursor::~PlanCursor() = default;

PlanCursor::Reader PlanCursor::OutputReader(uint32_t output) const {
  uint32_t column = plan_->columns()[output].index;
  return {column, ReaderFor(rows_.layout()[column])};
}

bool PlanCursor::Run(sqlite3_value** params) {
  // Each value is cast, as it is passed, for the column it is compared with.
  for (const Binder& binder : binders_) {
    binder.bind(binder, params, pool_);
  }
  return rows_.Open(core::Span<const core::exec::FilterValue>(
      filter_values_.data(), filter_values_.data() + filter_values_.size()));
}

void PlanCursor::BindAny(const Binder& binder,
                         sqlite3_value** params,
                         StringPool* pool) {
  ArgvFetcher fetcher;
  fetcher.params = params;
  const pipeline::PhysicalPlan::Param& param = *binder.param;
  auto cast =
      param.op.Is<core::In>()
          ? core::exec::GetFilterValueListCaster<ArgvFetcher>(param.type)
          : core::exec::GetFilterValueCaster<ArgvFetcher>(param.type, param.op);
  cast(fetcher, param.param, *pool, binder.value);
}

// Casts an integer for an equality on a column of type T as CastFilterValue
// does, without dispatching on the type or the comparison; any other value as
// BindAny does.
template <typename T>
void PlanCursor::BindIntegerEq(const Binder& binder,
                               sqlite3_value** params,
                               StringPool* pool) {
  sqlite3_value* value = params[binder.param->param];
  if (PERFETTO_UNLIKELY(sqlite::value::Type(value) != sqlite::Type::kInteger)) {
    return BindAny(binder, params, pool);
  }
  using Result = core::filter::CastFilterValueResult;
  using Cast = core::StorageType::VariantTypeAtIndex<T, Result::Value>;
  using Integer =
      std::conditional_t<std::is_same_v<T, core::Id>, uint32_t, Cast>;
  int64_t integer = sqlite::value::Int64(value);
  if constexpr (sizeof(Integer) < sizeof(int64_t)) {
    // Out of the column's range, it equals no value in it.
    if (integer < std::numeric_limits<Integer>::min() ||
        integer > std::numeric_limits<Integer>::max()) {
      binder.value->value = Result::NoneMatch();
      return;
    }
  }
  binder.value->value = Result::Valid(Cast{static_cast<Integer>(integer)});
}

PlanCursor::BindFn PlanCursor::BinderFor(
    const pipeline::PhysicalPlan::Param& param) {
  if (!param.op.Is<core::Eq>()) {
    return &BindAny;
  }
  switch (param.type.index()) {
    case core::StorageType::GetTypeIndex<core::Id>():
      return &BindIntegerEq<core::Id>;
    case core::StorageType::GetTypeIndex<core::Uint32>():
      return &BindIntegerEq<core::Uint32>;
    case core::StorageType::GetTypeIndex<core::Int32>():
      return &BindIntegerEq<core::Int32>;
    case core::StorageType::GetTypeIndex<core::Int64>():
      return &BindIntegerEq<core::Int64>;
    default:
      return &BindAny;
  }
}

}  // namespace perfetto::trace_processor
