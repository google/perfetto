/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/engine/dataframe_module.h"

#include <sqlite3.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/perfetto_sql/engine/dataframe_shared_storage.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/module_state_manager.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

std::optional<dataframe::Op> SqliteOpToDataframeOp(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return dataframe::Eq();
    case SQLITE_INDEX_CONSTRAINT_NE:
      return dataframe::Ne();
    case SQLITE_INDEX_CONSTRAINT_LT:
      return dataframe::Lt();
    case SQLITE_INDEX_CONSTRAINT_LE:
      return dataframe::Le();
    case SQLITE_INDEX_CONSTRAINT_GT:
      return dataframe::Gt();
    case SQLITE_INDEX_CONSTRAINT_GE:
      return dataframe::Ge();
    case SQLITE_INDEX_CONSTRAINT_GLOB:
      return dataframe::Glob();
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return dataframe::IsNull();
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return dataframe::IsNotNull();
    default:
      return std::nullopt;
  }
}

std::string ToSqliteCreateTableType(dataframe::StorageType type) {
  switch (type.index()) {
    case dataframe::StorageType::GetTypeIndex<dataframe::Id>():
    case dataframe::StorageType::GetTypeIndex<dataframe::Uint32>():
    case dataframe::StorageType::GetTypeIndex<dataframe::Int32>():
    case dataframe::StorageType::GetTypeIndex<dataframe::Int64>():
      return "INTEGER";
    case dataframe::StorageType::GetTypeIndex<dataframe::Double>():
      return "REAL";
    case dataframe::StorageType::GetTypeIndex<dataframe::String>():
      return "TEXT";
    default:
      PERFETTO_FATAL("Unimplemented");
  }
}

std::string CreateTableStmt(
    const std::vector<dataframe::Dataframe::ColumnSpec>& specs) {
  std::string create_stmt = "CREATE TABLE x(";
  for (const auto& spec : specs) {
    create_stmt += spec.name + " " + ToSqliteCreateTableType(spec.type) + ", ";
  }
  create_stmt += "PRIMARY KEY(id)) WITHOUT ROWID";
  return create_stmt;
}

}  // namespace

int DataframeModule::Create(sqlite3* db,
                            void* raw_ctx,
                            int argc,
                            const char* const* argv,
                            sqlite3_vtab** vtab,
                            char**) {
  // SQLite automatically should provide the first three arguments. And the
  // fourth argument should be the tag hash of the dataframe from the engine.
  PERFETTO_CHECK(argc == 4);

  std::optional<uint64_t> tag_hash = base::CStringToUInt64(argv[3]);
  PERFETTO_CHECK(tag_hash);

  auto* ctx = GetContext(raw_ctx);
  auto table = ctx->dataframe_shared_storage->Find(
      DataframeSharedStorage::Tag{*tag_hash});
  PERFETTO_CHECK(table);

  std::string create_stmt = CreateTableStmt(table->CreateColumnSpecs());
  if (int r = sqlite3_declare_vtab(db, create_stmt.c_str()); r != SQLITE_OK) {
    return r;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->dataframe = table.get();
  auto* state =
      ctx->OnCreate(argc, argv, std::make_unique<State>(std::move(table)));
  res->state = state;
  *vtab = res.release();
  return SQLITE_OK;
}

int DataframeModule::Destroy(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  sqlite::ModuleStateManager<DataframeModule>::OnDestroy(v->state);
  return SQLITE_OK;
}

int DataframeModule::Connect(sqlite3* db,
                             void* raw_ctx,
                             int argc,
                             const char* const* argv,
                             sqlite3_vtab** vtab,
                             char**) {
  // SQLite automatically should provide the first three arguments. And the
  // fourth argument should be the type of the tag of the dataframe which the
  // engine should always provide.
  PERFETTO_CHECK(argc >= 4);

  auto* vtab_state = GetContext(raw_ctx)->OnConnect(argc, argv);
  auto* state =
      sqlite::ModuleStateManager<DataframeModule>::GetState(vtab_state);
  std::string create_stmt =
      CreateTableStmt(state->dataframe->CreateColumnSpecs());
  if (int r = sqlite3_declare_vtab(db, create_stmt.c_str()); r != SQLITE_OK) {
    return r;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->dataframe = state->dataframe.get();
  res->state = vtab_state;
  *vtab = res.release();
  return SQLITE_OK;
}

int DataframeModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  sqlite::ModuleStateManager<DataframeModule>::OnDisconnect(v->state);
  return SQLITE_OK;
}

int DataframeModule::BestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
  auto* v = GetVtab(tab);

  std::vector<dataframe::FilterSpec> filter_specs;
  dataframe::LimitSpec limit_spec;
  filter_specs.reserve(static_cast<size_t>(info->nConstraint));
  for (int i = 0; i < info->nConstraint; ++i) {
    if (!info->aConstraint[i].usable) {
      continue;
    }
    sqlite3_value* rhs;
    int ret = sqlite3_vtab_rhs_value(info, i, &rhs);
    PERFETTO_CHECK(ret == SQLITE_OK || ret == SQLITE_NOTFOUND);

    int op = info->aConstraint[i].op;

    // Special handling for limit/offset values when we have a constant value.
    bool is_limit_offset = op == SQLITE_INDEX_CONSTRAINT_LIMIT ||
                           op == SQLITE_INDEX_CONSTRAINT_OFFSET;
    if (is_limit_offset && rhs &&
        sqlite::value::Type(rhs) == sqlite::Type::kInteger) {
      int64_t value = sqlite::value::Int64(rhs);
      if (value >= 0 && value <= std::numeric_limits<uint32_t>::max()) {
        auto cast = static_cast<uint32_t>(value);
        if (op == SQLITE_INDEX_CONSTRAINT_LIMIT) {
          limit_spec.limit = cast;
        } else {
          PERFETTO_DCHECK(op == SQLITE_INDEX_CONSTRAINT_OFFSET);
          limit_spec.offset = cast;
        }
      }
    }
    auto df_op = SqliteOpToDataframeOp(op);
    if (!df_op) {
      continue;
    }
    filter_specs.emplace_back(dataframe::FilterSpec{
        static_cast<uint32_t>(info->aConstraint[i].iColumn),
        static_cast<uint32_t>(i),
        *df_op,
        std::nullopt,
    });
  }

  bool should_sort_using_order_by = true;
  std::vector<dataframe::DistinctSpec> distinct_specs;
  if (info->nOrderBy > 0) {
    int vtab_distinct = sqlite3_vtab_distinct(info);
    switch (vtab_distinct) {
      case 0: /* normal sorting */
      // TODO(lalitm): add special handling for group by.
      case 1: /* group by */
        break;
      case 2: /* distinct */
      case 3: /* distinct + order by */ {
        uint64_t cols_used_it = info->colUsed;
        for (uint32_t i = 0; i < 64; ++i) {
          if (cols_used_it & 1u) {
            distinct_specs.push_back(dataframe::DistinctSpec{i});
          }
          cols_used_it >>= 1;
        }
        should_sort_using_order_by = (vtab_distinct == 3);
        break;
      }
      default:
        PERFETTO_FATAL("Unreachable");
    }
  }

  std::vector<dataframe::SortSpec> sort_specs;
  if (should_sort_using_order_by) {
    sort_specs.reserve(static_cast<size_t>(info->nOrderBy));
    for (int i = 0; i < info->nOrderBy; ++i) {
      sort_specs.emplace_back(dataframe::SortSpec{
          static_cast<uint32_t>(info->aOrderBy[i].iColumn),
          info->aOrderBy[i].desc ? dataframe::SortDirection::kDescending
                                 : dataframe::SortDirection::kAscending});
    }
  }
  info->orderByConsumed = true;

  SQLITE_ASSIGN_OR_RETURN(
      tab, auto plan,
      v->dataframe->PlanQuery(filter_specs, distinct_specs, sort_specs,
                              limit_spec, info->colUsed));
  for (const auto& c : filter_specs) {
    if (auto value_index = c.value_index; value_index) {
      info->aConstraintUsage[c.source_index].argvIndex =
          static_cast<int>(*value_index) + 1;
      info->aConstraintUsage[c.source_index].omit = true;
    }
  }
  info->idxStr = sqlite3_mprintf("%s", std::move(plan).Serialize().data());
  info->needToFreeIdxStr = true;
  info->estimatedCost = plan.estimated_cost();
  info->estimatedRows = plan.estimated_row_count();
  if (plan.max_row_count() <= 1) {
    info->idxFlags |= SQLITE_INDEX_SCAN_UNIQUE;
  }
  return SQLITE_OK;
}

int DataframeModule::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int DataframeModule::Close(sqlite3_vtab_cursor* cursor) {
  std::unique_ptr<Cursor> c(GetCursor(cursor));
  return SQLITE_OK;
}

int DataframeModule::Filter(sqlite3_vtab_cursor* cur,
                            int,
                            const char* idxStr,
                            int,
                            sqlite3_value** argv) {
  auto* v = GetVtab(cur->pVtab);
  auto* c = GetCursor(cur);
  if (idxStr != c->last_idx_str) {
    auto plan = dataframe::Dataframe::QueryPlan::Deserialize(idxStr);
    v->dataframe->PrepareCursor(plan, c->df_cursor);
    c->last_idx_str = idxStr;
  }
  SqliteValueFetcher fetcher{{}, argv};
  c->df_cursor->Execute(fetcher);
  return SQLITE_OK;
}

int DataframeModule::Next(sqlite3_vtab_cursor* cur) {
  GetCursor(cur)->df_cursor->Next();
  return SQLITE_OK;
}

int DataframeModule::Eof(sqlite3_vtab_cursor* cur) {
  return GetCursor(cur)->df_cursor->Eof();
}

int DataframeModule::Column(sqlite3_vtab_cursor* cur,
                            sqlite3_context* ctx,
                            int raw_n) {
  SqliteResultCallback visitor{{}, ctx};
  GetCursor(cur)->df_cursor->Cell(static_cast<uint32_t>(raw_n), visitor);
  return SQLITE_OK;
}

int DataframeModule::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor
