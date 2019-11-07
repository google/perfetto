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

#include <ctype.h>
#include <string.h>
#include <algorithm>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

namespace {

std::string TypeToString(SqlValue::Type type) {
  switch (type) {
    case SqlValue::Type::kString:
      return "STRING";
    case SqlValue::Type::kLong:
      return "BIG INT";
    case SqlValue::Type::kDouble:
      return "DOUBLE";
    case SqlValue::Type::kBytes:
      return "BLOB";
    case SqlValue::Type::kNull:
      PERFETTO_FATAL("Cannot map unknown column type");
  }
  PERFETTO_FATAL("Not reached");  // For gcc
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
  using ConstraintInfo = BestIndexInfo::ConstraintInfo;

  QueryConstraints in_qc;
  BestIndexInfo info;
  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (!cs.usable)
      continue;
    in_qc.AddConstraint(cs.iColumn, cs.op);

    ConstraintInfo c_info;
    c_info.qc_idx = static_cast<uint32_t>(in_qc.constraints().size() - 1);
    info.constraint_info.emplace_back(c_info);
  }

  for (int i = 0; i < idx->nOrderBy; i++) {
    int column = idx->aOrderBy[i].iColumn;
    bool desc = idx->aOrderBy[i].desc;
    in_qc.AddOrderBy(column, desc);
  }

  int ret = BestIndex(in_qc, &info);
  if (ret != SQLITE_OK)
    return ret;

  auto& cs_info = info.constraint_info;

  // Remove all the pruned terms from the constraints.
  {
    auto prune_fn = [](const ConstraintInfo& t) { return t.prune; };
    auto prune_cs_it = std::remove_if(cs_info.begin(), cs_info.end(), prune_fn);
    cs_info.erase(prune_cs_it, cs_info.end());
  }

  idx->orderByConsumed = info.prune_order_by || info.sqlite_omit_order_by;
  idx->estimatedCost = info.estimated_cost;

  uint32_t in_qc_idx = 0;
  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& c = idx->aConstraint[i];
    if (c.usable) {
      auto cs_fn = [in_qc_idx](const ConstraintInfo& t) {
        return t.qc_idx == in_qc_idx;
      };
      auto it = std::find_if(cs_info.begin(), cs_info.end(), cs_fn);

      // If the iterator no longer exists, we must have pruned it.
      if (it == cs_info.end()) {
        idx->aConstraintUsage[i].omit = true;
      } else {
        idx->aConstraintUsage[i].argvIndex =
            static_cast<int>(std::distance(cs_info.begin(), it)) + 1;
        idx->aConstraintUsage[i].omit = it->sqlite_omit;
      }
      in_qc_idx++;
    }
  }

  QueryConstraints out_qc;
  for (const auto& c_info : cs_info) {
    const auto& c = in_qc.constraints()[c_info.qc_idx];
    out_qc.AddConstraint(c.iColumn, c.op);
  }
  if (!info.prune_order_by) {
    for (const auto& o : in_qc.order_by()) {
      out_qc.AddOrderBy(o.iColumn, o.desc);
    }
  }

  auto out_qc_str = out_qc.ToNewSqlite3String();
  if (SqliteTable::debug) {
    PERFETTO_LOG(
        "[%s::BestIndex] constraints=%s orderByConsumed=%d estimatedCost=%d",
        name_.c_str(), out_qc_str.get(), idx->orderByConsumed,
        info.estimated_cost);
  }

  idx->idxStr = out_qc_str.release();
  idx->needToFreeIdxStr = true;
  idx->idxNum = ++best_index_num_;

  return SQLITE_OK;
}

int SqliteTable::FindFunction(const char*, FindFunctionFn, void**) {
  return 0;
}

int SqliteTable::Update(int, sqlite3_value**, sqlite3_int64*) {
  return SQLITE_READONLY;
}

const QueryConstraints& SqliteTable::ParseConstraints(int idxNum,
                                                      const char* idxStr,
                                                      int argc) {
  bool cache_hit = true;
  if (idxNum != qc_hash_) {
    qc_cache_ = QueryConstraints::FromString(idxStr);
    qc_hash_ = idxNum;
    cache_hit = false;
  }
  if (SqliteTable::debug) {
    PERFETTO_LOG("[%s::ParseConstraints] constraints=%s argc=%d cache_hit=%d",
                 name_.c_str(), idxStr, argc, cache_hit);
  }
  return qc_cache_;
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
