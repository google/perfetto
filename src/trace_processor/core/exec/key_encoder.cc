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

#include "src/trace_processor/core/exec/key_encoder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/layout_column.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
std::optional<uint32_t> KeyEncoder::Encode(
    const RowBatch& batch,
    const std::vector<uint32_t>& columns) {
  kinds_.resize(columns.size());
  bool first = !columns.empty() && !kinds_[0];
  for (uint32_t k = 0; k < columns.size(); ++k) {
    const ColumnView& column = batch.column(columns[k]);
    std::optional<Kind> kind;
    switch (column.kind()) {
      case ColumnView::Kind::kSequence:
        kind = Kind::kInteger;
        break;
      case ColumnView::Kind::kFlat:
        switch (column.type().index()) {
          case StorageType::GetTypeIndex<Uint32>():
          case StorageType::GetTypeIndex<Int32>():
          case StorageType::GetTypeIndex<Int64>():
            kind = Kind::kInteger;
            break;
          case StorageType::GetTypeIndex<Double>():
            kind = Kind::kDouble;
            break;
          case StorageType::GetTypeIndex<String>():
            kind = Kind::kString;
            break;
          default:
            break;
        }
        break;
      case ColumnView::Kind::kVariant:
        break;
    }
    if (!kind || (kinds_[k] && *kinds_[k] != *kind)) {
      return k;
    }
    kinds_[k] = kind;
  }
  if (first) {
    std::vector<RowLayout::Column> layout;
    for (const std::optional<Kind>& kind : kinds_) {
      switch (*kind) {
        case Kind::kInteger:
          layout.push_back({RowLayout::Type::kInt64, true});
          break;
        case Kind::kDouble:
          layout.push_back({RowLayout::Type::kDouble, true});
          break;
        case Kind::kString:
          layout.push_back({RowLayout::Type::kUint32, false});
          break;
      }
    }
    layout_ = RowLayout(layout);
  }

  uint32_t count = batch.size();
  bytes_.resize(size_t{layout_.stride()} * count);
  auto* rows = reinterpret_cast<uint8_t*>(bytes_.data());
  for (uint32_t k = 0; k < columns.size(); ++k) {
    const ColumnView& column = batch.column(columns[k]);
    const RowLayout::Slot& slot = layout_.slot(k);
    if (column.kind() == ColumnView::Kind::kSequence) {
      WriteLayoutColumn<int64_t>(column, slot, count, rows,
                                 [](uint32_t index) { return int64_t{index}; });
      continue;
    }
    const void* data = column.data();
    switch (column.type().index()) {
      case StorageType::GetTypeIndex<Uint32>():
        WriteLayoutColumn<int64_t>(
            column, slot, count, rows, [data](uint32_t index) {
              return int64_t{static_cast<const uint32_t*>(data)[index]};
            });
        break;
      case StorageType::GetTypeIndex<Int32>():
        WriteLayoutColumn<int64_t>(
            column, slot, count, rows, [data](uint32_t index) {
              return int64_t{static_cast<const int32_t*>(data)[index]};
            });
        break;
      case StorageType::GetTypeIndex<Int64>():
        WriteLayoutColumn<int64_t>(
            column, slot, count, rows, [data](uint32_t index) {
              return static_cast<const int64_t*>(data)[index];
            });
        break;
      case StorageType::GetTypeIndex<Double>():
        WriteLayoutColumn<double>(
            column, slot, count, rows, [data](uint32_t index) {
              return static_cast<const double*>(data)[index];
            });
        break;
      case StorageType::GetTypeIndex<String>(): {
        // Not nullable: the null id is 0.
        RowSelection selection = column.selection();
        const BitVector* validity = column.validity();
        const auto* ids = static_cast<const StringPool::Id*>(data);
        RowLayout::Write<uint32_t>(
            slot, count,
            [&](uint32_t row, uint32_t* out) {
              uint32_t index = selection.GetIndex(row);
              bool present = !validity || validity->is_set(index);
              *out = present ? ids[index].raw_id() : 0;
              return true;
            },
            rows);
        break;
      }
      default:
        PERFETTO_FATAL("Unknown key type");
    }
  }
  return std::nullopt;
}

}  // namespace perfetto::trace_processor::core::exec
