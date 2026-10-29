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
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/string_ranks.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/layout_column.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Records `column`'s string at each row `selection` keeps, and whether it has
// one, adding each string to those to rank.
void AppendStrings(const ColumnView& column,
                   const Selection& selection,
                   FlexVector<StringPool::Id>* ids,
                   BitVector* present,
                   StringRanks* ranks) {
  const auto* data = static_cast<const StringPool::Id*>(column.data());
  const BitVector* validity = column.validity();
  uint32_t start = column.start();
  for (uint32_t row = 0; row < selection.size(); ++row) {
    uint32_t index = start + selection[row];
    bool valid = !validity || validity->is_set(index);
    ids->push_back(valid ? data[index] : StringPool::Id::Null());
    present->push_back(valid);
    if (valid) {
      ranks->Add(data[index]);
    }
  }
}

}  // namespace

std::optional<SortedRows::KeyType> SortedRows::GetKeyType(
    const ColumnView& column,
    bool strings) {
  switch (column.kind()) {
    case ColumnView::Kind::kSequence:
      return KeyType{RowLayout::Type::kUint32, false};
    case ColumnView::Kind::kFlat:
      switch (column.type().index()) {
        case StorageType::GetTypeIndex<Uint32>():
          return KeyType{RowLayout::Type::kUint32, false};
        case StorageType::GetTypeIndex<Int32>():
          return KeyType{RowLayout::Type::kInt32, false};
        case StorageType::GetTypeIndex<Int64>():
          return KeyType{RowLayout::Type::kInt64, false};
        case StorageType::GetTypeIndex<Double>():
          return KeyType{RowLayout::Type::kDouble, false};
        case StorageType::GetTypeIndex<String>():
          if (strings) {
            return KeyType{RowLayout::Type::kUint32, true};
          }
          return std::nullopt;
        default:
          return std::nullopt;
      }
    case ColumnView::Kind::kVariant:
      return std::nullopt;
  }
  PERFETTO_FATAL("Unknown column kind");
}

base::Status SortedRows::Append(const RowBatch& in,
                                const std::vector<SortKey>& keys,
                                const StringPool* pool) {
  uint32_t count = in.size();
  if (count == 0) {
    return base::OkStatus();
  }
  bool first = types_.empty() && !keys.empty();
  for (uint32_t k = 0; k < keys.size(); ++k) {
    std::optional<KeyType> type =
        GetKeyType(in.column(keys[k].column), pool != nullptr);
    if (!type || (!first && (types_[k].layout != type->layout ||
                             types_[k].string != type->string))) {
      return base::ErrStatus(
          "key %u must be a column of numbers, or of strings, of one type",
          k + 1);
    }
  }
  if (first) {
    // Record the types only after every key is checked. Otherwise an error
    // could leave types set with no matching row layout. Every key is laid
    // out as nullable, because nullability can change from batch to batch.
    std::vector<RowLayout::Column> columns;
    for (uint32_t k = 0; k < keys.size(); ++k) {
      types_.push_back(*GetKeyType(in.column(keys[k].column), true));
      columns.push_back({types_[k].layout, true, keys[k].descending});
    }
    layout_ = RowLayout(columns);
    strings_.resize(keys.size());
    pool_ = pool;
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
    if (types_[k].string) {
      // Written by Sort(), once every string is in and ranked.
      AppendStrings(column, in.selection(), &strings_[k].ids,
                    &strings_[k].present, &ranks_);
      continue;
    }
    switch (types_[k].layout) {
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
  if (ranks_.size()) {
    ranks_.Rank(*pool_);
  }
  for (uint32_t k = 0; k < types_.size(); ++k) {
    if (!types_[k].string) {
      continue;
    }
    const Strings& strings = strings_[k];
    RowLayout::Write<uint32_t>(
        layout_.slot(k), rows,
        [&](uint32_t row, uint32_t* out) {
          if (!strings.present.is_set(row)) {
            return false;
          }
          *out = *ranks_.Find(strings.ids[row]);
          return true;
        },
        keys_.data());
  }
  order_.resize(rows);
  std::iota(order_.data(), order_.data() + rows, 0u);
  if (layout_.stride()) {
    Span<uint32_t> order(order_.data(), order_.data() + rows);
    core::ops::SortRowLayout(
        Span<const uint8_t>(keys_.data(), keys_.data() + keys_.size()),
        layout_.stride(), &order);
  }
}

uint32_t SortedRows::Gather(RowBatch* out, uint32_t at, Context& context) {
  uint32_t count = std::min(kMaxBatchRows, rows_.size() - at);
  const uint32_t* begin = order_.data() + at;
  return rows_.Gather(out, Span<const uint32_t>(begin, begin + count), context);
}

void SortedRows::Clear() {
  types_.clear();
  strings_.clear();
  ranks_.Clear();
  pool_ = nullptr;
  keys_.clear();
  rows_.Clear();
  order_.clear();
}

}  // namespace perfetto::trace_processor::core::exec
