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

#include "src/trace_processor/sqlite/sqlite_table.h"

#include <string.h>
#include <algorithm>
#include <cinttypes>
#include <map>

#include "perfetto/base/logging.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {

namespace {

std::string TypeToSqlString(SqlValue::Type type) {
  switch (type) {
    case SqlValue::Type::kString:
      return "TEXT";
    case SqlValue::Type::kLong:
      return "BIGINT";
    case SqlValue::Type::kDouble:
      return "DOUBLE";
    case SqlValue::Type::kBytes:
      return "BLOB";
    case SqlValue::Type::kNull:
      PERFETTO_FATAL("Cannot map unknown column type");
  }
  PERFETTO_FATAL("Not reached");  // For gcc
}

std::string OpToDebugString(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return "=";
    case SQLITE_INDEX_CONSTRAINT_NE:
      return "!=";
    case SQLITE_INDEX_CONSTRAINT_GE:
      return ">=";
    case SQLITE_INDEX_CONSTRAINT_GT:
      return ">";
    case SQLITE_INDEX_CONSTRAINT_LE:
      return "<=";
    case SQLITE_INDEX_CONSTRAINT_LT:
      return "<";
    case SQLITE_INDEX_CONSTRAINT_LIKE:
      return "like";
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return "is null";
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return "is not null";
    case SQLITE_INDEX_CONSTRAINT_GLOB:
      return "glob";
    case SQLITE_INDEX_CONSTRAINT_LIMIT:
      return "limit";
    case SQLITE_INDEX_CONSTRAINT_OFFSET:
      return "offset";
    case SqliteTable::CustomFilterOpcode::kSourceGeqOpCode:
      return "source_geq";
    default:
      PERFETTO_FATAL("Operator to string conversion not impemented for %d", op);
  }
}

void ConstraintsToString(const QueryConstraints& qc,
                         const SqliteTable::Schema& schema,
                         std::string& out) {
  bool is_first = true;
  for (const auto& cs : qc.constraints()) {
    if (!is_first) {
      out.append(",");
    }
    out.append(schema.columns()[static_cast<size_t>(cs.column)].name());
    out.append(" ");
    out.append(OpToDebugString(cs.op));
    is_first = false;
  }
}

void OrderByToString(const QueryConstraints& qc,
                     const SqliteTable::Schema& schema,
                     std::string& out) {
  bool is_first = true;
  for (const auto& ob : qc.order_by()) {
    if (!is_first) {
      out.append(",");
    }
    out.append(schema.columns()[static_cast<size_t>(ob.iColumn)].name());
    out.append(" ");
    out.append(std::to_string(ob.desc));
    is_first = false;
  }
}

std::string QcDebugStr(const QueryConstraints& qc,
                       const SqliteTable::Schema& schema) {
  std::string str_result;
  str_result.reserve(512);

  str_result.append("C");
  str_result.append(std::to_string(qc.constraints().size()));
  str_result.append(",");
  ConstraintsToString(qc, schema, str_result);
  str_result.append(";");

  str_result.append("O");
  str_result.append(std::to_string(qc.order_by().size()));
  str_result.append(",");
  OrderByToString(qc, schema, str_result);
  str_result.append(";");

  str_result.append("U");
  str_result.append(std::to_string(qc.cols_used()));

  return str_result;
}

void WriteQueryConstraintsToMetatrace(metatrace::Record* r,
                                      const QueryConstraints& qc,
                                      const SqliteTable::Schema& schema) {
  r->AddArg("constraint_count", std::to_string(qc.constraints().size()));
  std::string constraints;
  ConstraintsToString(qc, schema, constraints);
  r->AddArg("constraints", constraints);
  r->AddArg("order_by_count", std::to_string(qc.order_by().size()));
  std::string order_by;
  OrderByToString(qc, schema, order_by);
  r->AddArg("order_by", order_by);
  r->AddArg("columns_used", std::to_string(qc.cols_used()));
}

}  // namespace

// static
bool SqliteTable::debug = false;

SqliteTable::SqliteTable() = default;
SqliteTable::~SqliteTable() = default;

int SqliteTable::OpenInternal(sqlite3_vtab_cursor** ppCursor) {
  // Freed in xClose().
  *ppCursor = static_cast<sqlite3_vtab_cursor*>(CreateCursor().release());
  return SQLITE_OK;
}

int SqliteTable::BestIndexInternal(sqlite3_index_info* idx) {
  QueryConstraints qc(idx->colUsed);

  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (!cs.usable)
      continue;
    qc.AddConstraint(cs.iColumn, cs.op, i);
  }

  for (int i = 0; i < idx->nOrderBy; i++) {
    int column = idx->aOrderBy[i].iColumn;
    bool desc = idx->aOrderBy[i].desc;
    qc.AddOrderBy(column, desc);
  }

  int ret = ModifyConstraints(&qc);
  if (ret != SQLITE_OK)
    return ret;

  BestIndexInfo info;
  info.estimated_cost = idx->estimatedCost;
  info.estimated_rows = idx->estimatedRows;
  info.sqlite_omit_constraint.resize(qc.constraints().size());

  ret = BestIndex(qc, &info);

  if (ret != SQLITE_OK)
    return ret;

  idx->orderByConsumed = qc.order_by().empty() || info.sqlite_omit_order_by;
  idx->estimatedCost = info.estimated_cost;
  idx->estimatedRows = info.estimated_rows;

  // First pass: mark all constraints as omitted to ensure that any pruned
  // constraints are not checked for by SQLite.
  for (int i = 0; i < idx->nConstraint; ++i) {
    auto& u = idx->aConstraintUsage[i];
    u.omit = true;
  }

  // Second pass: actually set the correct omit and index values for all
  // retained constraints.
  for (uint32_t i = 0; i < qc.constraints().size(); ++i) {
    auto& u = idx->aConstraintUsage[qc.constraints()[i].a_constraint_idx];
    u.omit = info.sqlite_omit_constraint[i];
    u.argvIndex = static_cast<int>(i) + 1;
  }

  PERFETTO_TP_TRACE(
      metatrace::Category::QUERY, "SQLITE_TABLE_BEST_INDEX",
      [&](metatrace::Record* r) {
        r->AddArg("name", name_);
        WriteQueryConstraintsToMetatrace(r, qc, schema());
        r->AddArg("order_by_consumed", std::to_string(idx->orderByConsumed));
        r->AddArg("estimated_cost", std::to_string(idx->estimatedCost));
        r->AddArg("estimated_rows",
                  std::to_string(static_cast<int64_t>(idx->estimatedRows)));
      });

  auto out_qc_str = qc.ToNewSqlite3String();
  if (SqliteTable::debug) {
    PERFETTO_LOG(
        "[%s::BestIndex] constraints=%s orderByConsumed=%d estimatedCost=%f "
        "estimatedRows=%" PRId64,
        name_.c_str(), QcDebugStr(qc, schema()).c_str(), idx->orderByConsumed,
        idx->estimatedCost, static_cast<int64_t>(idx->estimatedRows));
  }

  idx->idxStr = out_qc_str.release();
  idx->needToFreeIdxStr = true;
  idx->idxNum = ++best_index_num_;

  return SQLITE_OK;
}

int SqliteTable::ModifyConstraints(QueryConstraints*) {
  return SQLITE_OK;
}

int SqliteTable::FindFunction(const char*, FindFunctionFn*, void**) {
  return 0;
}

int SqliteTable::Update(int, sqlite3_value**, sqlite3_int64*) {
  return SQLITE_READONLY;
}

bool SqliteTable::ReadConstraints(int idxNum, const char* idxStr, int argc) {
  bool cache_hit = true;
  if (idxNum != qc_hash_) {
    qc_cache_ = QueryConstraints::FromString(idxStr);
    qc_hash_ = idxNum;
    cache_hit = false;
  }

  PERFETTO_TP_TRACE(metatrace::Category::QUERY, "SQLITE_TABLE_READ_CONSTRAINTS",
                    [&](metatrace::Record* r) {
                      r->AddArg("cache_hit", std::to_string(cache_hit));
                      r->AddArg("name", name_);
                      WriteQueryConstraintsToMetatrace(r, qc_cache_, schema_);
                      r->AddArg("raw_constraints", idxStr);
                      r->AddArg("argc", std::to_string(argc));
                    });

  // Logging this every ReadConstraints just leads to log spam on joins making
  // it unusable. Instead, only print this out when we miss the cache (which
  // happens precisely when the constraint set from SQLite changes.)
  if (SqliteTable::debug && !cache_hit) {
    PERFETTO_LOG("[%s::ParseConstraints] constraints=%s argc=%d", name_.c_str(),
                 QcDebugStr(qc_cache_, schema_).c_str(), argc);
  }
  return cache_hit;
}

SqliteTable::Cursor::Cursor(SqliteTable* table) : table_(table) {
  // This is required to prevent us from leaving this field uninitialised if
  // we ever move construct the Cursor.
  pVtab = table;
}
SqliteTable::Cursor::~Cursor() = default;

int SqliteTable::Cursor::RowId(sqlite3_int64*) {
  return SQLITE_ERROR;
}

SqliteTable::Column::Column(size_t index,
                            std::string name,
                            SqlValue::Type type,
                            bool hidden)
    : index_(index), name_(name), type_(type), hidden_(hidden) {}

SqliteTable::Schema::Schema(std::vector<Column> columns,
                            std::vector<size_t> primary_keys)
    : columns_(std::move(columns)), primary_keys_(std::move(primary_keys)) {
  for (size_t i = 0; i < columns_.size(); i++) {
    PERFETTO_CHECK(columns_[i].index() == i);
  }
  for (auto key : primary_keys_) {
    PERFETTO_CHECK(key < columns_.size());
  }
}

SqliteTable::Schema::Schema() = default;
SqliteTable::Schema::Schema(const Schema&) = default;
SqliteTable::Schema& SqliteTable::Schema::operator=(const Schema&) = default;

std::string SqliteTable::Schema::ToCreateTableStmt() const {
  std::string stmt = "CREATE TABLE x(";
  for (size_t i = 0; i < columns_.size(); ++i) {
    const Column& col = columns_[i];
    stmt += " " + col.name();

    if (col.type() != SqlValue::Type::kNull) {
      stmt += " " + TypeToSqlString(col.type());
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
