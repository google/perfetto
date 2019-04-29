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

#include "src/trace_processor/table.h"

#include <ctype.h>
#include <string.h>
#include <algorithm>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

namespace {

std::string TypeToString(Table::ColumnType type) {
  switch (type) {
    case Table::ColumnType::kString:
      return "STRING";
    case Table::ColumnType::kUint:
      return "UNSIGNED INT";
    case Table::ColumnType::kLong:
      return "BIG INT";
    case Table::ColumnType::kInt:
      return "INT";
    case Table::ColumnType::kDouble:
      return "DOUBLE";
    case Table::ColumnType::kUnknown:
      PERFETTO_FATAL("Cannot map unknown column type");
  }
  PERFETTO_FATAL("Not reached");  // For gcc
}

}  // namespace

// static
bool Table::debug = false;

Table::Table() = default;
Table::~Table() = default;

int Table::OpenInternal(sqlite3_vtab_cursor** ppCursor) {
  // Freed in xClose().
  *ppCursor = static_cast<sqlite3_vtab_cursor*>(CreateCursor().release());
  return SQLITE_OK;
}

int Table::BestIndexInternal(sqlite3_index_info* idx) {
  QueryConstraints query_constraints;

  for (int i = 0; i < idx->nOrderBy; i++) {
    int column = idx->aOrderBy[i].iColumn;
    bool desc = idx->aOrderBy[i].desc;
    query_constraints.AddOrderBy(column, desc);
  }

  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (!cs.usable)
      continue;
    query_constraints.AddConstraint(cs.iColumn, cs.op);

    // argvIndex is 1-based so use the current size of the vector.
    int argv_index = static_cast<int>(query_constraints.constraints().size());
    idx->aConstraintUsage[i].argvIndex = argv_index;
  }

  BestIndexInfo info;
  info.omit.resize(query_constraints.constraints().size());

  int ret = BestIndex(query_constraints, &info);

  if (Table::debug) {
    PERFETTO_LOG(
        "[%s::BestIndex] constraints=%s orderByConsumed=%d estimatedCost=%d",
        name_.c_str(), query_constraints.ToNewSqlite3String().get(),
        info.order_by_consumed, info.estimated_cost);
  }

  if (ret != SQLITE_OK)
    return ret;

  idx->orderByConsumed = info.order_by_consumed;
  idx->estimatedCost = info.estimated_cost;

  size_t j = 0;
  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (cs.usable)
      idx->aConstraintUsage[i].omit = info.omit[j++];
  }

  if (!info.order_by_consumed)
    query_constraints.ClearOrderBy();

  idx->idxStr = query_constraints.ToNewSqlite3String().release();
  idx->needToFreeIdxStr = true;
  idx->idxNum = ++best_index_num_;

  return SQLITE_OK;
}

int Table::FindFunction(const char*, FindFunctionFn, void**) {
  return 0;
}

int Table::Update(int, sqlite3_value**, sqlite3_int64*) {
  return SQLITE_READONLY;
}

const QueryConstraints& Table::ParseConstraints(int idxNum,
                                                const char* idxStr,
                                                int argc) {
  bool cache_hit = true;
  if (idxNum != qc_hash_) {
    qc_cache_ = QueryConstraints::FromString(idxStr);
    qc_hash_ = idxNum;
    cache_hit = false;
  }
  if (Table::debug) {
    PERFETTO_LOG("[%s::ParseConstraints] constraints=%s argc=%d cache_hit=%d",
                 name_.c_str(), idxStr, argc, cache_hit);
  }
  return qc_cache_;
}

Table::Cursor::Cursor(Table* table) : table_(table) {
  // This is required to prevent us from leaving this field uninitialised if
  // we ever move construct the Cursor.
  pVtab = table;
}
Table::Cursor::~Cursor() = default;

int Table::Cursor::RowId(sqlite3_int64*) {
  return SQLITE_ERROR;
}

Table::Column::Column(size_t index,
                      std::string name,
                      ColumnType type,
                      bool hidden)
    : index_(index), name_(name), type_(type), hidden_(hidden) {}

Table::Schema::Schema(std::vector<Column> columns,
                      std::vector<size_t> primary_keys)
    : columns_(std::move(columns)), primary_keys_(std::move(primary_keys)) {
  for (size_t i = 0; i < columns_.size(); i++) {
    PERFETTO_CHECK(columns_[i].index() == i);
  }
  for (auto key : primary_keys_) {
    PERFETTO_CHECK(key < columns_.size());
  }
}

Table::Schema::Schema() = default;
Table::Schema::Schema(const Schema&) = default;
Table::Schema& Table::Schema::operator=(const Schema&) = default;

std::string Table::Schema::ToCreateTableStmt() const {
  std::string stmt = "CREATE TABLE x(";
  for (size_t i = 0; i < columns_.size(); ++i) {
    const Column& col = columns_[i];
    stmt += " " + col.name();

    if (col.type() != ColumnType::kUnknown) {
      stmt += " " + TypeToString(col.type());
    } else if (std::find(primary_keys_.begin(), primary_keys_.end(), i) !=
               primary_keys_.end()) {
      PERFETTO_FATAL("Unknown type for primary key column %s",
                     col.name().c_str());
    }
    if (col.hidden()) {
      stmt += " HIDDEN";
    }
    stmt += ",";
  }
  stmt += " PRIMARY KEY(";
  for (size_t i = 0; i < primary_keys_.size(); i++) {
    if (i != 0)
      stmt += ", ";
    stmt += columns_[primary_keys_[i]].name();
  }
  stmt += ")) WITHOUT ROWID;";
  return stmt;
}

}  // namespace trace_processor
}  // namespace perfetto
