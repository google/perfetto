/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/storage_schema.h"

#include "src/trace_processor/row_iterators.h"

namespace perfetto {
namespace trace_processor {

StorageSchema::StorageSchema() = default;
StorageSchema::StorageSchema(std::vector<std::unique_ptr<Column>> columns)
    : columns_(std::move(columns)) {}

Table::Schema StorageSchema::ToTableSchema(std::vector<std::string> pkeys) {
  std::vector<Table::Column> columns;
  size_t i = 0;
  for (const auto& col : columns_)
    columns.emplace_back(i++, col->name(), col->GetType(), col->hidden());

  std::vector<size_t> primary_keys;
  for (const auto& p_key : pkeys)
    primary_keys.emplace_back(ColumnIndexFromName(p_key));
  return Table::Schema(std::move(columns), std::move(primary_keys));
}

size_t StorageSchema::ColumnIndexFromName(const std::string& name) {
  auto p = [name](const std::unique_ptr<Column>& col) {
    return name == col->name();
  };
  auto it = std::find_if(columns_.begin(), columns_.end(), p);
  return static_cast<size_t>(std::distance(columns_.begin(), it));
}

StorageSchema::Column::Column(std::string col_name, bool hidden)
    : col_name_(col_name), hidden_(hidden) {}
StorageSchema::Column::~Column() = default;

}  // namespace trace_processor
}  // namespace perfetto
