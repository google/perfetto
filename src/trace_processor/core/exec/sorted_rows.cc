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
#include <cstring>
#include <numeric>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/string_ranks.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/layout_column.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/ops.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Records `column`'s string at each row `selection` keeps, and whether it has
// one, adding each string to those to rank.
void AppendStrings(const ColumnView& column,
                   const Selection& selection,
                   OperatorState& owner,
                   RunVector<StringPool::Id>* ids,
                   RunBits* present,
                   RunMap* ranks,
                   RunVector<StringPool::Id>* ranked) {
  bool sparse = column.kind() == ColumnView::Kind::kSparse;
  const auto* data = static_cast<const StringPool::Id*>(
      sparse ? column.sparse_data() : column.data());
  const BitVector* validity = column.validity();
  const uint32_t* popcount = sparse ? column.popcount() : nullptr;
  uint32_t start = column.start();
  uint32_t count = selection.size();
  const uint16_t* indices = selection.prefix() ? nullptr : selection.indices();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = start + (indices ? indices[row] : row);
    bool valid = !validity || validity->is_set(index);
    if (valid && sparse) {
      index =
          filter::IndexToStorageIndex<SparseNull>(index, validity, popcount);
    }
    StringPool::Id id = valid ? data[index] : StringPool::Id::Null();
    ids->push_back(owner, id);
    present->push_back(owner, valid);
    if (valid) {
      uint32_t before = ranks->size();
      ranks->FindOrInsert(owner, id.raw_id(), 0);
      if (ranks->size() != before) {
        ranked->push_back(owner, id);
      }
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
    case ColumnView::Kind::kSparse:
      switch (column.type().index()) {
        case StorageType::GetTypeIndex<Id>():
          // Only a sparse column stores ids: as their positions in storage.
          return KeyType{RowLayout::Type::kUint32, false};
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
                                base::Span<const SortKey> keys,
                                const StringPool* pool,
                                OperatorState& owner) {
  return Store(in, keys, pool, owner, nullptr);
}

base::Status SortedRows::AppendLaidOut(const RowBatch& in,
                                       base::Span<const SortKey> keys,
                                       const StringPool* pool,
                                       OperatorState& owner,
                                       const uint8_t* laid_out) {
  return Store(in, keys, pool, owner, laid_out);
}

base::Status SortedRows::Store(const RowBatch& in,
                               base::Span<const SortKey> keys,
                               const StringPool* pool,
                               OperatorState& owner,
                               const uint8_t* laid_out) {
  if (in.size() == 0) {
    return base::OkStatus();
  }
  RETURN_IF_ERROR(CheckKeys(in, keys, pool, owner));
  uint64_t at = uint64_t{rows_.size()} * stride_;
  keys_.resize(owner, at + uint64_t{in.size()} * stride_);
  if (laid_out) {
    memcpy(keys_.data() + at, laid_out, uint64_t{in.size()} * stride_);
  } else {
    LayOutKeys(in, keys, keys_.data() + at);
  }
  for (uint32_t k = 0; k < keys.size(); ++k) {
    if (types_[k].string) {
      // Laid out by Sort(), once every string is in and ranked.
      AppendStrings(in.column(keys[k].column), in.selection(), owner,
                    &strings_[k].ids, &strings_[k].present, &ranks_, &ranked_);
    }
  }
  return rows_.Append(in, owner);
}

base::Status SortedRows::CheckKeys(const RowBatch& in,
                                   base::Span<const SortKey> keys,
                                   const StringPool* pool,
                                   OperatorState& owner) {
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
    columns_.clear();
    for (uint32_t k = 0; k < keys.size(); ++k) {
      types_.push_back(owner, *GetKeyType(in.column(keys[k].column), true));
      columns_.push_back(owner, {types_[k].layout, true, keys[k].descending});
    }
    slots_.resize(owner, keys.size());
    stride_ =
        RowLayout::LayOut(columns_.data(), columns_.size(), slots_.data());
    uint32_t had = strings_.size();
    strings_.resize(owner, keys.size());
    for (uint32_t k = had; k < keys.size(); ++k) {
      strings_[k] = Strings();
    }
    pool_ = pool;
  }
  return base::OkStatus();
}

void SortedRows::LayOutKeys(const RowBatch& in,
                            base::Span<const SortKey> keys,
                            uint8_t* rows) const {
  for (uint32_t k = 0; k < keys.size(); ++k) {
    const ColumnView& column = in.column(keys[k].column);
    const RowLayout::Slot& slot = slots_[k];
    bool sparse = column.kind() == ColumnView::Kind::kSparse;
    if (column.kind() == ColumnView::Kind::kSequence ||
        (sparse && column.type().Is<Id>())) {
      WriteLayoutColumn<uint32_t>(column, in.selection(), slot, rows,
                                  [](uint32_t index) { return index; });
      continue;
    }
    if (types_[k].string) {
      continue;
    }
    const void* data = sparse ? column.sparse_data() : column.data();
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
}

std::optional<StringPool::Id> SortedRows::string(uint32_t key,
                                                 uint32_t row) const {
  const Strings& strings = strings_[key];
  if (!strings.present.is_set(row)) {
    return std::nullopt;
  }
  return strings.ids[row];
}

void SortedRows::Sort(OperatorState& owner) {
  uint32_t rows = rows_.size();
  if (ranked_.size()) {
    StringRanks::Sort(*pool_, ranked_.data(), ranked_.size());
    for (uint32_t rank = 0; rank < ranked_.size(); ++rank) {
      *ranks_.Find(ranked_[rank].raw_id()) = rank;
    }
  }
  for (uint32_t k = 0; k < types_.size(); ++k) {
    if (!types_[k].string) {
      continue;
    }
    const Strings& strings = strings_[k];
    RowLayout::Write<uint32_t>(
        slots_[k], rows,
        [&](uint32_t row, uint32_t* out) {
          if (!strings.present.is_set(row)) {
            return false;
          }
          *out = *ranks_.Find(strings.ids[row].raw_id());
          return true;
        },
        keys_.data());
  }
  order_.resize(owner, rows);
  std::iota(order_.data(), order_.data() + rows, 0u);
  if (stride_) {
    base::Span<uint32_t> order(order_.data(), order_.data() + rows);
    core::ops::SortRowLayout(
        base::Span<const uint8_t>(keys_.data(), keys_.data() + keys_.size()),
        stride_, &order);
  }
}

uint32_t SortedRows::Gather(RowBatch* out, uint32_t at, OperatorState& owner) {
  auto count = std::min(kMaxBatchRows, order_.size() - at);
  const uint32_t* begin = order_.data() + at;
  return rows_.Gather(out, base::Span<const uint32_t>(begin, begin + count),
                      owner);
}

uint32_t SortedRows::GatherInPlace(RowBatch* out,
                                   uint32_t at,
                                   OperatorState& owner) {
  auto count = std::min(kMaxBatchRows, order_.size() - at);
  const uint32_t* begin = order_.data() + at;
  return rows_.GatherInPlace(
      out, base::Span<const uint32_t>(begin, begin + count), owner);
}

void SortedRows::Place(OperatorState& owner) {
  base::Span<const uint32_t> order(order_.data(), order_.data() + size());
  placed_ = rows_.Place(order, &in_place_, owner);
}

void SortedRows::Clear() {
  types_.clear();
  for (Strings& strings : strings_) {
    strings.ids.clear();
    strings.present.clear();
  }
  ranks_.clear();
  ranked_.clear();
  pool_ = nullptr;
  keys_.clear();
  rows_.Clear();
  order_.clear();
  in_place_.batches.clear();
  placed_ = false;
  in_place_.batch.clear();
  in_place_.row.clear();
}

}  // namespace perfetto::trace_processor::core::exec
