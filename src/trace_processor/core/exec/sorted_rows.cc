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

#include "src/trace_processor/core/exec/sorted_rows.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/layout_column.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

std::optional<RowLayout::Type> LayoutType(const ColumnView& column) {
  switch (column.kind()) {
    case ColumnView::Kind::kSequence:
      return RowLayout::Type::kUint32;
    case ColumnView::Kind::kFlat:
      switch (column.type().index()) {
        case StorageType::GetTypeIndex<Uint32>():
          return RowLayout::Type::kUint32;
        case StorageType::GetTypeIndex<Int32>():
          return RowLayout::Type::kInt32;
        case StorageType::GetTypeIndex<Int64>():
          return RowLayout::Type::kInt64;
        case StorageType::GetTypeIndex<Double>():
          return RowLayout::Type::kDouble;
        default:
          return std::nullopt;
      }
    case ColumnView::Kind::kVariant:
      return std::nullopt;
  }
  PERFETTO_FATAL("Unknown column kind");
}

}  // namespace

base::Status SortedRows::Append(const RowBatch& in,
                                const std::vector<SortKey>& keys) {
  uint32_t count = in.size();
  if (count == 0) {
    return base::OkStatus();
  }
  if (types_.empty()) {
    types_.resize(keys.size());
  }
  bool first = !types_.empty() && !types_[0];
  for (uint32_t k = 0; k < keys.size(); ++k) {
    std::optional<RowLayout::Type> type = LayoutType(in.column(keys[k].column));
    if (!type || (types_[k] && *types_[k] != *type)) {
      return base::ErrStatus("key %u must be a column of numbers of one type",
                             k + 1);
    }
  }
  if (first) {
    // Record types only after every key is valid, so an error cannot leave
    // partially initialized types without a matching row layout.
    // Nullability is only known batch by batch.
    std::vector<RowLayout::Column> columns;
    for (uint32_t k = 0; k < keys.size(); ++k) {
      types_[k] = LayoutType(in.column(keys[k].column));
      columns.push_back({*types_[k], true, keys[k].descending});
    }
    layout_ = RowLayout(columns);
  }

  uint64_t at = uint64_t{rows_.size()} * layout_.stride();
  keys_.resize(at + uint64_t{count} * layout_.stride());
  uint8_t* rows = keys_.data() + at;
  for (uint32_t k = 0; k < keys.size(); ++k) {
    const ColumnView& column = in.column(keys[k].column);
    const RowLayout::Slot& slot = layout_.slot(k);
    if (column.kind() == ColumnView::Kind::kSequence) {
      WriteLayoutColumn<uint32_t>(column, in.selection(), slot, rows,
                                  [](uint32_t index) { return index; });
      continue;
    }
    const void* data = column.data();
    switch (*types_[k]) {
      case RowLayout::Type::kUint32:
        WriteLayoutColumn<uint32_t>(
            column, in.selection(), slot, rows, [data](uint32_t index) {
              return static_cast<const uint32_t*>(data)[index];
            });
        break;
      case RowLayout::Type::kInt32:
        WriteLayoutColumn<int32_t>(
            column, in.selection(), slot, rows, [data](uint32_t index) {
              return static_cast<const int32_t*>(data)[index];
            });
        break;
      case RowLayout::Type::kInt64:
        WriteLayoutColumn<int64_t>(
            column, in.selection(), slot, rows, [data](uint32_t index) {
              return static_cast<const int64_t*>(data)[index];
            });
        break;
      case RowLayout::Type::kDouble:
        WriteLayoutColumn<double>(
            column, in.selection(), slot, rows, [data](uint32_t index) {
              return static_cast<const double*>(data)[index];
            });
        break;
    }
  }
  return rows_.Append(in);
}

void SortedRows::Sort() {
  uint32_t rows = rows_.size();
  order_.resize(rows);
  std::iota(order_.data(), order_.data() + rows, 0u);
  if (layout_.stride()) {
    Span<uint32_t> order(order_.data(), order_.data() + rows);
    core::ops::SortRowLayout(
        Span<const uint8_t>(keys_.data(), keys_.data() + keys_.size()),
        layout_.stride(), &order);
  }
}

uint32_t SortedRows::View(RowBatch* out, uint32_t at, Context& context) {
  uint32_t count = std::min(kMaxBatchRows, rows_.size() - at);
  const uint32_t* begin = order_.data() + at;
  return rows_.View(out, Span<const uint32_t>(begin, begin + count), context);
}

void SortedRows::Clear() {
  std::fill(types_.begin(), types_.end(), std::nullopt);
  keys_.clear();
  rows_.Clear();
  order_.clear();
}

}  // namespace perfetto::trace_processor::core::exec
