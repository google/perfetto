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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_COLUMN_RESULTS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_COLUMN_RESULTS_H_

#include <sqlite3.h>

#include <cstdint>
#include <limits>
#include <type_traits>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"

namespace perfetto::trace_processor {

using ColumnResultFn = void (*)(sqlite3_context*,
                                StringPool*,
                                const core::exec::ColumnView&,
                                uint32_t);

namespace column_results_internal {

using core::exec::ColumnView;
using core::exec::Variant;

inline void ResultString(sqlite3_context* ctx,
                         StringPool* pool,
                         StringPool::Id id) {
  if (id.is_null()) {
    sqlite::result::Null(ctx);
    return;
  }
  // Interned strings live as long as the pool, and end in a zero byte.
  auto str = pool->Get(id);
  sqlite::result::StaticTerminatedString(ctx, str.c_str(), str.size());
}

template <typename T>
void ResultValue(sqlite3_context* ctx, StringPool* pool, T value) {
  if constexpr (std::is_same_v<T, double>) {
    sqlite::result::Double(ctx, value);
  } else if constexpr (std::is_same_v<T, StringPool::Id>) {
    ResultString(ctx, pool, value);
  } else {
    sqlite::result::Long(ctx, value);
  }
}

inline void ResultVariant(sqlite3_context* ctx,
                          StringPool* pool,
                          const ColumnView& view,
                          uint32_t row) {
  Variant cell = view.At<Variant>(row);
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

// Checks for nulls per row: a column can have them in one batch but not
// another.
template <typename T>
void ResultFlat(sqlite3_context* ctx,
                StringPool* pool,
                const ColumnView& view,
                uint32_t row) {
  uint32_t index = view.Index(row);
  if (!view.IsValid(row)) {
    return sqlite::result::Null(ctx);
  }
  ResultValue(ctx, pool, static_cast<const T*>(view.data())[index]);
}

inline void ResultSequence(sqlite3_context* ctx,
                           StringPool*,
                           const ColumnView& view,
                           uint32_t row) {
  uint32_t index = view.Index(row);
  if (!view.IsValid(row)) {
    return sqlite::result::Null(ctx);
  }
  sqlite::result::Long(ctx, index);
}

// The slot of a kSparse column's storage holding batch row `row`, or
// UINT32_MAX if the row is null.
inline uint32_t SparseSlot(const ColumnView& view, uint32_t row) {
  return core::filter::IndexToStorageIndex<core::SparseNull>(
      view.Index(row), view.validity(), view.popcount());
}

template <typename T>
void ResultSparse(sqlite3_context* ctx,
                  StringPool* pool,
                  const ColumnView& view,
                  uint32_t row) {
  uint32_t slot = SparseSlot(view, row);
  if (slot == std::numeric_limits<uint32_t>::max()) {
    return sqlite::result::Null(ctx);
  }
  ResultValue(ctx, pool, static_cast<const T*>(view.sparse_data())[slot]);
}

// A sparse Id column stores no values: each is its slot.
inline void ResultSparseSlot(sqlite3_context* ctx,
                             StringPool*,
                             const ColumnView& view,
                             uint32_t row) {
  uint32_t slot = SparseSlot(view, row);
  if (slot == std::numeric_limits<uint32_t>::max()) {
    return sqlite::result::Null(ctx);
  }
  sqlite::result::Long(ctx, slot);
}

inline ColumnResultFn ColumnResultFor(const ColumnView& view) {
  if (view.kind() == ColumnView::Kind::kVariant) {
    return &ResultVariant;
  }
  if (view.kind() == ColumnView::Kind::kSequence) {
    return &ResultSequence;
  }
  core::StorageType type = view.type();
  if (view.kind() == ColumnView::Kind::kSparse) {
    if (!view.sparse_data())
      return &ResultSparseSlot;
    if (type.Is<core::Uint32>())
      return &ResultSparse<uint32_t>;
    if (type.Is<core::Int32>())
      return &ResultSparse<int32_t>;
    if (type.Is<core::Int64>())
      return &ResultSparse<int64_t>;
    if (type.Is<core::Double>())
      return &ResultSparse<double>;
    if (type.Is<core::String>())
      return &ResultSparse<StringPool::Id>;
    PERFETTO_FATAL("Unexpected column type");
  }
  if (type.Is<core::Uint32>())
    return &ResultFlat<uint32_t>;
  if (type.Is<core::Int32>())
    return &ResultFlat<int32_t>;
  if (type.Is<core::Int64>())
    return &ResultFlat<int64_t>;
  if (type.Is<core::Double>())
    return &ResultFlat<double>;
  if (type.Is<core::String>())
    return &ResultFlat<StringPool::Id>;
  PERFETTO_FATAL("Unexpected column type");
}

}  // namespace column_results_internal

using column_results_internal::ColumnResultFor;

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_COLUMN_RESULTS_H_
