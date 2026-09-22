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

#include "src/trace_processor/perfetto_sql/engine/pipeline_dataframe_builder.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::ColumnView;
using core::exec::RowBatch;
using core::exec::Variant;
using dataframe::AdhocDataframeBuilder;

constexpr uint32_t kNoRow = std::numeric_limits<uint32_t>::max();

// Pushes rows [0, count) of a flat column holding T. Returns the first row the
// builder rejected, or kNoRow.
template <typename T>
uint32_t PushFlat(AdhocDataframeBuilder& builder,
                  uint32_t column,
                  const ColumnView& view,
                  uint32_t count) {
  const auto* data = static_cast<const T*>(view.data());
  const core::BitVector* validity = view.validity();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = view.selection().GetIndex(row);
    if (validity && !validity->is_set(index)) {
      builder.PushNull(column);
    } else if constexpr (std::is_same_v<T, int32_t>) {
      if (!builder.PushNonNull(column, int64_t{data[index]})) {
        return row;
      }
    } else if (!builder.PushNonNull(column, data[index])) {
      return row;
    }
  }
  return kNoRow;
}

// As PushFlat, for a column whose value is the row it sits at.
uint32_t PushSequence(AdhocDataframeBuilder& builder,
                      uint32_t column,
                      const ColumnView& view,
                      uint32_t count) {
  const core::BitVector* validity = view.validity();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = view.selection().GetIndex(row);
    if (validity && !validity->is_set(index)) {
      builder.PushNull(column);
    } else if (!builder.PushNonNull(column, index)) {
      return row;
    }
  }
  return kNoRow;
}

// As PushFlat, for a column carrying a type per row.
uint32_t PushVariants(AdhocDataframeBuilder& builder,
                      uint32_t column,
                      const ColumnView& view,
                      uint32_t count) {
  for (uint32_t row = 0; row < count; ++row) {
    Variant cell = view.Value<Variant>(row);
    bool ok = true;
    switch (cell.type) {
      case Variant::Type::kNull:
        builder.PushNull(column);
        break;
      case Variant::Type::kInt64:
        ok = builder.PushNonNull(column, cell.AsInt64());
        break;
      case Variant::Type::kDouble:
        ok = builder.PushNonNull(column, cell.AsDouble());
        break;
      case Variant::Type::kString:
        ok = builder.PushNonNull(column, cell.AsString());
        break;
    }
    if (!ok) {
      return row;
    }
  }
  return kNoRow;
}

uint32_t PushColumn(AdhocDataframeBuilder& builder,
                    uint32_t column,
                    const ColumnView& view,
                    uint32_t count) {
  if (view.kind() == ColumnView::Kind::kVariant) {
    return PushVariants(builder, column, view, count);
  }
  if (view.kind() == ColumnView::Kind::kSequence) {
    return PushSequence(builder, column, view, count);
  }
  core::StorageType type = view.type();
  if (type.Is<core::Uint32>()) {
    return PushFlat<uint32_t>(builder, column, view, count);
  }
  if (type.Is<core::Int32>()) {
    return PushFlat<int32_t>(builder, column, view, count);
  }
  if (type.Is<core::Int64>()) {
    return PushFlat<int64_t>(builder, column, view, count);
  }
  if (type.Is<core::Double>()) {
    return PushFlat<double>(builder, column, view, count);
  }
  if (type.Is<core::String>()) {
    return PushFlat<StringPool::Id>(builder, column, view, count);
  }
  PERFETTO_FATAL("Unexpected column type");
}

}  // namespace

base::StatusOr<dataframe::Dataframe> BuildDataframeFromPipeline(
    StringPool* pool,
    std::vector<std::string> column_names,
    const pipeline::PhysicalPlan& plan,
    std::string_view error_context,
    const AdhocDataframeBuilder::Options& options) {
  PERFETTO_CHECK(column_names.size() == plan.columns().size());
  AdhocDataframeBuilder builder(std::move(column_names), pool, options);

  const core::exec::Source& source = plan.source();
  std::unique_ptr<core::exec::OperatorState> state = source.MakeState();
  RowBatch batch;
  while (source.GetData(batch, *state)) {
    // A batch goes in a column at a time, but the row which fails the build is
    // the first one to hold a value which does not fit, and of its values the
    // first: the one which would have failed had the rows gone in whole.
    uint32_t failed_row = kNoRow;
    base::Status failure = base::OkStatus();
    for (uint32_t i = 0; i < plan.columns().size(); ++i) {
      uint32_t row = PushColumn(
          builder, i, batch.column(plan.columns()[i].index), batch.size());
      if (row < failed_row) {
        failed_row = row;
        failure = builder.status();
      }
    }
    if (failed_row != kNoRow) {
      return base::ErrStatus("%.*s: %s", static_cast<int>(error_context.size()),
                             error_context.data(), failure.c_message());
    }
  }
  RETURN_IF_ERROR(source.status(*state));
  return std::move(builder).Build();
}

}  // namespace perfetto::trace_processor
