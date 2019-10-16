/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/db/column.h"

#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

Column::Column(const Column& column,
               Table* table,
               uint32_t col_idx,
               uint32_t row_map_idx)
    : Column(column.name_,
             column.type_,
             column.flags_,
             table,
             col_idx,
             row_map_idx,
             column.sparse_vector_) {}

Column::Column(const char* name,
               ColumnType type,
               uint32_t flags,
               Table* table,
               uint32_t col_idx,
               uint32_t row_map_idx,
               void* sparse_vector)
    : type_(type),
      sparse_vector_(sparse_vector),
      name_(name),
      flags_(flags),
      table_(table),
      col_idx_(col_idx),
      row_map_idx_(row_map_idx),
      string_pool_(table->string_pool_) {}

Column Column::IdColumn(Table* table, uint32_t col_idx, uint32_t row_map_idx) {
  return Column("id", ColumnType::kId, Flag::kId | Flag::kSorted, table,
                col_idx, row_map_idx, nullptr);
}

void Column::FilterInto(FilterOp op, SqlValue value, RowMap* iv) const {
  // TODO(lalitm): add special logic here to deal with kId and kSorted flags.
  switch (op) {
    case FilterOp::kLt:
      iv->RemoveIf([this, value](uint32_t row) { return Get(row) >= value; });
      break;
    case FilterOp::kEq:
      iv->RemoveIf([this, value](uint32_t row) { return Get(row) != value; });
      break;
    case FilterOp::kGt:
      iv->RemoveIf([this, value](uint32_t row) { return Get(row) <= value; });
      break;
    case FilterOp::kNe:
      iv->RemoveIf([this, value](uint32_t row) { return Get(row) == value; });
      break;
    case FilterOp::kLe:
      iv->RemoveIf([this, value](uint32_t row) { return Get(row) > value; });
      break;
    case FilterOp::kGe:
      iv->RemoveIf([this, value](uint32_t row) { return Get(row) < value; });
      break;
    case FilterOp::kIsNull:
      iv->RemoveIf([this](uint32_t row) {
        return Get(row).type != SqlValue::Type::kNull;
      });
      break;
    case FilterOp::kIsNotNull:
      iv->RemoveIf([this](uint32_t row) {
        return Get(row).type == SqlValue::Type::kNull;
      });
      break;
  }
}

const RowMap& Column::row_map() const {
  return table_->row_maps_[row_map_idx_];
}

}  // namespace trace_processor
}  // namespace perfetto
