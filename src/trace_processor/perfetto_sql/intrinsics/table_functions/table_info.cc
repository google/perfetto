/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/table_info.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/runtime_table.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"

namespace perfetto::trace_processor {
namespace tables {

PerfettoTableInfoTable::~PerfettoTableInfoTable() = default;

}  // namespace tables

namespace {

using TableInfoTable = tables::PerfettoTableInfoTable;

std::vector<TableInfoTable::Row> GetColInfoRows(
    const std::vector<ColumnLegacy>& cols,
    StringPool* pool) {
  std::vector<TableInfoTable::Row> rows;
  for (const ColumnLegacy& col : cols) {
    if (col.IsHidden()) {
      continue;
    }
    TableInfoTable::Row row;
    row.name = pool->InternString(col.name());
    switch (col.col_type()) {
      case ColumnType::kString:
        row.col_type = pool->InternString("string");
        break;
      case ColumnType::kInt64:
        row.col_type = pool->InternString("int64");
        break;
      case ColumnType::kInt32:
        row.col_type = pool->InternString("int32");
        break;
      case ColumnType::kUint32:
        row.col_type = pool->InternString("uint32");
        break;
      case ColumnType::kDouble:
        row.col_type = pool->InternString("double");
        break;
      case ColumnType::kId:
        row.col_type = pool->InternString("id");
        break;
      case ColumnType::kDummy:
        row.col_type = pool->InternString("dummy");
        break;
    }
    if (col.IsSetId()) {
      row.col_type = pool->InternString("set id");
    }
    row.nullable = col.IsNullable();
    row.sorted = col.IsSorted();
    rows.push_back(row);
  }
  return rows;
}

}  // namespace

TableInfo::TableInfo(StringPool* string_pool, const PerfettoSqlEngine* engine)
    : string_pool_(string_pool), engine_(engine) {}

base::StatusOr<std::unique_ptr<Table>> TableInfo::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 1);
  if (arguments[0].type != SqlValue::kString) {
    return base::ErrStatus("perfetto_table_info takes table name as a string.");
  }

  std::string table_name = arguments[0].AsString();
  auto table = std::make_unique<TableInfoTable>(string_pool_);
  auto table_name_id = string_pool_->InternString(table_name.c_str());

  // Find static table
  const Table* static_table = engine_->GetStaticTableOrNull(table_name);
  if (static_table) {
    for (auto& row : GetColInfoRows(static_table->columns(), string_pool_)) {
      row.table_name = table_name_id;
      table->Insert(row);
    }
    return std::unique_ptr<Table>(std::move(table));
  }

  // Find runtime table
  const RuntimeTable* runtime_table =
      engine_->GetRuntimeTableOrNull(table_name);
  if (runtime_table) {
    for (auto& row : GetColInfoRows(runtime_table->columns(), string_pool_)) {
      row.table_name = table_name_id;
      table->Insert(row);
    }
    return std::unique_ptr<Table>(std::move(table));
  }

  return base::ErrStatus("Perfetto table '%s' not found.", table_name.c_str());
}

Table::Schema TableInfo::CreateSchema() {
  return TableInfoTable::ComputeStaticSchema();
}

std::string TableInfo::TableName() {
  return TableInfoTable::Name();
}

uint32_t TableInfo::EstimateRowCount() {
  return 1;
}

}  // namespace perfetto::trace_processor
