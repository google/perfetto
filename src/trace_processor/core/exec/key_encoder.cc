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

#include "perfetto/base/compiler.h"
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
namespace {

void WriteSequence(const ColumnView& column,
                   RowLayout::Slot slot,
                   uint32_t count,
                   uint8_t* rows) {
  WriteLayoutColumn<int64_t>(column, slot, count, rows,
                             [](uint32_t index) { return int64_t{index}; });
}

template <typename Stored>
void WriteInteger(const ColumnView& column,
                  RowLayout::Slot slot,
                  uint32_t count,
                  uint8_t* rows) {
  const auto* data = static_cast<const Stored*>(column.data());
  WriteLayoutColumn<int64_t>(column, slot, count, rows, [data](uint32_t index) {
    return int64_t{data[index]};
  });
}

void WriteDouble(const ColumnView& column,
                 RowLayout::Slot slot,
                 uint32_t count,
                 uint8_t* rows) {
  const auto* data = static_cast<const double*>(column.data());
  WriteLayoutColumn<double>(column, slot, count, rows,
                            [data](uint32_t index) { return data[index]; });
}

// Not nullable: the null id is 0.
void WriteString(const ColumnView& column,
                 RowLayout::Slot slot,
                 uint32_t count,
                 uint8_t* rows) {
  FlatColumnReader<StringPool::Id> ids(column);
  RowLayout::Write<uint32_t>(
      slot, count,
      [&](uint32_t row, uint32_t* out) {
        StringPool::Id id;
        *out = ids.Read(row, &id) ? id.raw_id() : 0;
        return true;
      },
      rows);
}

uint16_t ShapeOf(const ColumnView& column) {
  return static_cast<uint16_t>(static_cast<uint32_t>(column.kind()) << 8 |
                               column.type().index());
}

}  // namespace

std::optional<uint32_t> KeyEncoder::Encode(
    const RowBatch& batch,
    const std::vector<uint32_t>& columns) {
  bool same = key_columns_.size() == columns.size();
  for (uint32_t k = 0; same && k < columns.size(); ++k) {
    same = key_columns_[k].shape == ShapeOf(batch.column(columns[k]));
  }
  if (PERFETTO_UNLIKELY(!same)) {
    return Classify(batch, columns);
  }
  WriteKeys(batch, columns);
  return std::nullopt;
}

PERFETTO_ALWAYS_INLINE void KeyEncoder::WriteKeys(
    const RowBatch& batch,
    const std::vector<uint32_t>& columns) {
  uint32_t count = batch.size();
  bytes_.resize(size_t{layout_.stride()} * count);
  uint8_t* rows = bytes_.data();
  const uint32_t* index = columns.data();
  for (const KeyColumn *c = key_columns_.data(), *end = c + key_columns_.size();
       c != end; ++c, ++index) {
    c->writer(batch.column(*index), c->slot, count, rows);
  }
}

PERFETTO_NO_INLINE std::optional<uint32_t> KeyEncoder::Classify(
    const RowBatch& batch,
    const std::vector<uint32_t>& columns) {
  bool first = kinds_.empty();
  kinds_.resize(columns.size());
  key_columns_.resize(columns.size());
  for (uint32_t k = 0; k < columns.size(); ++k) {
    const ColumnView& column = batch.column(columns[k]);
    std::optional<Kind> kind;
    Writer writer = nullptr;
    switch (column.kind()) {
      case ColumnView::Kind::kSequence:
        kind = Kind::kInteger;
        writer = &WriteSequence;
        break;
      case ColumnView::Kind::kFlat:
        switch (column.type().index()) {
          case StorageType::GetTypeIndex<Uint32>():
            kind = Kind::kInteger;
            writer = &WriteInteger<uint32_t>;
            break;
          case StorageType::GetTypeIndex<Int32>():
            kind = Kind::kInteger;
            writer = &WriteInteger<int32_t>;
            break;
          case StorageType::GetTypeIndex<Int64>():
            kind = Kind::kInteger;
            writer = &WriteInteger<int64_t>;
            break;
          case StorageType::GetTypeIndex<Double>():
            kind = Kind::kDouble;
            writer = &WriteDouble;
            break;
          case StorageType::GetTypeIndex<String>():
            kind = Kind::kString;
            writer = &WriteString;
            break;
          default:
            break;
        }
        break;
      case ColumnView::Kind::kVariant:
        break;
    }
    if (!kind || (!first && kinds_[k] != *kind)) {
      // Nothing is kept of a batch which fails: the next is checked in full,
      // and laid out if none has been yet.
      key_columns_.clear();
      if (first) {
        kinds_.clear();
      }
      return k;
    }
    kinds_[k] = *kind;
    key_columns_[k].shape = ShapeOf(column);
    key_columns_[k].writer = writer;
  }
  if (first) {
    std::vector<RowLayout::Column> layout;
    for (Kind kind : kinds_) {
      switch (kind) {
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
  for (uint32_t k = 0; k < key_columns_.size(); ++k) {
    key_columns_[k].slot = layout_.slot(k);
  }
  WriteKeys(batch, columns);
  return std::nullopt;
}

}  // namespace perfetto::trace_processor::core::exec
