/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/operators/stdlib_docs_operator.h"

#include <sqlite3.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/simple_json_serializer.h"
#include "src/trace_processor/util/sql_modules.h"
#include "src/trace_processor/util/stdlib_doc_parser.h"

namespace perfetto::trace_processor {

namespace {

const char* ValueText(sqlite3_value* v) {
  return reinterpret_cast<const char*>(sqlite3_value_text(v));
}

const std::string* FindModuleSql(const PerfettoSqlEngine* engine,
                                 const std::string& module_key) {
  std::string package_name = sql_modules::GetPackageName(module_key);
  const auto* package = engine->FindPackage(package_name);
  if (!package) {
    return nullptr;
  }
  auto* module_file = package->modules.Find(module_key);
  if (!module_file) {
    return nullptr;
  }
  return &module_file->sql;
}

template <typename Entry>
std::string SerializeEntries(const std::vector<Entry>& entries) {
  return json::SerializeJson([&](json::JsonValueSerializer&& writer) {
    std::move(writer).WriteArray([&](json::JsonArraySerializer& array) {
      for (const auto& e : entries) {
        array.AppendDict([&](json::JsonDictSerializer& dict) {
          dict.AddString("name", e.name);
          dict.AddString("type", e.type);
          dict.AddString("description", e.description);
        });
      }
    });
  });
}

constexpr char kModulesSchema[] = R"(
  CREATE TABLE x(
    module TEXT,
    package TEXT,
    PRIMARY KEY(module)
  ) WITHOUT ROWID
)";

constexpr char kTablesSchema[] = R"(
  CREATE TABLE x(
    name TEXT,
    type TEXT,
    description TEXT,
    exposed INTEGER,
    cols TEXT,
    module TEXT HIDDEN,
    PRIMARY KEY(name)
  ) WITHOUT ROWID
)";

constexpr char kFunctionsSchema[] = R"(
  CREATE TABLE x(
    name TEXT,
    description TEXT,
    exposed INTEGER,
    is_table_function INTEGER,
    return_type TEXT,
    return_description TEXT,
    args TEXT,
    cols TEXT,
    module TEXT HIDDEN,
    PRIMARY KEY(name)
  ) WITHOUT ROWID
)";

constexpr char kMacrosSchema[] = R"(
  CREATE TABLE x(
    name TEXT,
    description TEXT,
    exposed INTEGER,
    return_type TEXT,
    return_description TEXT,
    args TEXT,
    module TEXT HIDDEN,
    PRIMARY KEY(name)
  ) WITHOUT ROWID
)";

}  // namespace

// ============================================================================
// StdlibDocsModulesOperator
// ============================================================================

int StdlibDocsModulesOperator::Connect(sqlite3* db,
                                       void* raw_ctx,
                                       int,
                                       const char* const*,
                                       sqlite3_vtab** vtab,
                                       char**) {
  if (int r = sqlite3_declare_vtab(db, kModulesSchema); r != SQLITE_OK) {
    return r;
  }
  auto res = std::make_unique<Vtab>();
  res->engine = GetContext(raw_ctx);
  *vtab = res.release();
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::BestIndex(sqlite3_vtab*,
                                         sqlite3_index_info* info) {
  info->estimatedCost = 1000.0;
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cur) {
  *cur = std::make_unique<Cursor>().release();
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::Close(sqlite3_vtab_cursor* cur) {
  delete GetCursor(cur);
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::Filter(sqlite3_vtab_cursor* cur,
                                      int,
                                      const char*,
                                      int,
                                      sqlite3_value**) {
  auto* c = GetCursor(cur);
  auto* engine = GetVtab(c->pVtab)->engine;
  c->rows.clear();
  c->index = 0;
  engine->ForEachModule([&](const std::string& pkg, const std::string& mod) {
    c->rows.emplace_back(mod, pkg);
  });
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::Next(sqlite3_vtab_cursor* cur) {
  GetCursor(cur)->index++;
  return SQLITE_OK;
}

int StdlibDocsModulesOperator::Eof(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  return c->index >= c->rows.size();
}

int StdlibDocsModulesOperator::Column(sqlite3_vtab_cursor* cur,
                                      sqlite3_context* ctx,
                                      int col) {
  auto* c = GetCursor(cur);
  const auto& [module, package] = c->rows[c->index];
  switch (col) {
    case Column::kModuleName:
      sqlite::result::StaticString(ctx, module.c_str());
      return SQLITE_OK;
    case Column::kPackage:
      sqlite::result::StaticString(ctx, package.c_str());
      return SQLITE_OK;
    default:
      PERFETTO_FATAL("Unknown column %d", col);
  }
}

int StdlibDocsModulesOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

// ============================================================================
// StdlibDocsTablesOperator
// ============================================================================

int StdlibDocsTablesOperator::Connect(sqlite3* db,
                                      void* raw_ctx,
                                      int,
                                      const char* const*,
                                      sqlite3_vtab** vtab,
                                      char**) {
  if (int r = sqlite3_declare_vtab(db, kTablesSchema); r != SQLITE_OK) {
    return r;
  }
  auto res = std::make_unique<Vtab>();
  res->engine = GetContext(raw_ctx);
  *vtab = res.release();
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::BestIndex(sqlite3_vtab*,
                                        sqlite3_index_info* info) {
  base::Status status = sqlite::utils::ValidateFunctionArguments(
      info, 1, [](size_t col) { return col == Column::kModuleArg; });
  if (!status.ok()) {
    return SQLITE_CONSTRAINT;
  }
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cur) {
  *cur = std::make_unique<Cursor>().release();
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::Close(sqlite3_vtab_cursor* cur) {
  delete GetCursor(cur);
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::Filter(sqlite3_vtab_cursor* cur,
                                     int,
                                     const char*,
                                     int argc,
                                     sqlite3_value** argv) {
  auto* c = GetCursor(cur);
  c->rows.clear();
  c->index = 0;
  PERFETTO_DCHECK(argc == 1);
  const char* text = ValueText(argv[0]);
  if (!text) {
    return SQLITE_OK;
  }
  std::string module_key = text;
  const std::string* sql = FindModuleSql(GetVtab(c->pVtab)->engine, module_key);
  if (!sql) {
    return sqlite::utils::SetError(
        GetVtab(c->pVtab),
        base::ErrStatus("Module not found: %s", module_key.c_str()));
  }
  auto parsed = stdlib_doc::ParseStdlibModule(
      sql->c_str(), static_cast<uint32_t>(sql->size()));
  for (const auto& error : parsed.errors) {
    PERFETTO_DLOG("perfetto_stdlib_tables: parse error in '%s': %s",
                  module_key.c_str(), error.c_str());
  }
  c->rows = std::move(parsed.table_views);
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::Next(sqlite3_vtab_cursor* cur) {
  GetCursor(cur)->index++;
  return SQLITE_OK;
}

int StdlibDocsTablesOperator::Eof(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  return c->index >= c->rows.size();
}

int StdlibDocsTablesOperator::Column(sqlite3_vtab_cursor* cur,
                                     sqlite3_context* ctx,
                                     int col) {
  auto* c = GetCursor(cur);
  const auto& row = c->rows[c->index];
  switch (col) {
    case Column::kName:
      sqlite::result::StaticString(ctx, row.name.c_str());
      return SQLITE_OK;
    case Column::kSqlType:
      sqlite::result::StaticString(ctx, row.type.c_str());
      return SQLITE_OK;
    case Column::kDescription:
      sqlite::result::StaticString(ctx, row.description.c_str());
      return SQLITE_OK;
    case Column::kExposed:
      sqlite::result::Long(ctx, row.exposed ? 1 : 0);
      return SQLITE_OK;
    case Column::kCols:
      sqlite::result::TransientString(ctx,
                                      SerializeEntries(row.columns).c_str());
      return SQLITE_OK;
    case Column::kModuleArg:
      return SQLITE_OK;
    default:
      PERFETTO_FATAL("Unknown column %d", col);
  }
}

int StdlibDocsTablesOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

// ============================================================================
// StdlibDocsFunctionsOperator
// ============================================================================

int StdlibDocsFunctionsOperator::Connect(sqlite3* db,
                                         void* raw_ctx,
                                         int,
                                         const char* const*,
                                         sqlite3_vtab** vtab,
                                         char**) {
  if (int r = sqlite3_declare_vtab(db, kFunctionsSchema); r != SQLITE_OK) {
    return r;
  }
  auto res = std::make_unique<Vtab>();
  res->engine = GetContext(raw_ctx);
  *vtab = res.release();
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::BestIndex(sqlite3_vtab*,
                                           sqlite3_index_info* info) {
  base::Status status = sqlite::utils::ValidateFunctionArguments(
      info, 1, [](size_t col) { return col == Column::kModuleArg; });
  if (!status.ok()) {
    return SQLITE_CONSTRAINT;
  }
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::Open(sqlite3_vtab*,
                                      sqlite3_vtab_cursor** cur) {
  *cur = std::make_unique<Cursor>().release();
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::Close(sqlite3_vtab_cursor* cur) {
  delete GetCursor(cur);
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::Filter(sqlite3_vtab_cursor* cur,
                                        int,
                                        const char*,
                                        int argc,
                                        sqlite3_value** argv) {
  auto* c = GetCursor(cur);
  c->rows.clear();
  c->index = 0;
  PERFETTO_DCHECK(argc == 1);
  const char* text = ValueText(argv[0]);
  if (!text) {
    return SQLITE_OK;
  }
  std::string module_key = text;
  const std::string* sql = FindModuleSql(GetVtab(c->pVtab)->engine, module_key);
  if (!sql) {
    return sqlite::utils::SetError(
        GetVtab(c->pVtab),
        base::ErrStatus("Module not found: %s", module_key.c_str()));
  }
  auto parsed = stdlib_doc::ParseStdlibModule(
      sql->c_str(), static_cast<uint32_t>(sql->size()));
  for (const auto& error : parsed.errors) {
    PERFETTO_DLOG("perfetto_stdlib_functions: parse error in '%s': %s",
                  module_key.c_str(), error.c_str());
  }
  c->rows = std::move(parsed.functions);
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::Next(sqlite3_vtab_cursor* cur) {
  GetCursor(cur)->index++;
  return SQLITE_OK;
}

int StdlibDocsFunctionsOperator::Eof(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  return c->index >= c->rows.size();
}

int StdlibDocsFunctionsOperator::Column(sqlite3_vtab_cursor* cur,
                                        sqlite3_context* ctx,
                                        int col) {
  auto* c = GetCursor(cur);
  const auto& row = c->rows[c->index];
  switch (col) {
    case Column::kName:
      sqlite::result::StaticString(ctx, row.name.c_str());
      return SQLITE_OK;
    case Column::kDescription:
      sqlite::result::StaticString(ctx, row.description.c_str());
      return SQLITE_OK;
    case Column::kExposed:
      sqlite::result::Long(ctx, row.exposed ? 1 : 0);
      return SQLITE_OK;
    case Column::kIsTableFunction:
      sqlite::result::Long(ctx, row.is_table_function ? 1 : 0);
      return SQLITE_OK;
    case Column::kReturnType:
      sqlite::result::StaticString(ctx, row.return_type.c_str());
      return SQLITE_OK;
    case Column::kReturnDescription:
      sqlite::result::StaticString(ctx, row.return_description.c_str());
      return SQLITE_OK;
    case Column::kArgs:
      sqlite::result::TransientString(ctx, SerializeEntries(row.args).c_str());
      return SQLITE_OK;
    case Column::kCols:
      sqlite::result::TransientString(ctx,
                                      SerializeEntries(row.columns).c_str());
      return SQLITE_OK;
    case Column::kModuleArg:
      return SQLITE_OK;
    default:
      PERFETTO_FATAL("Unknown column %d", col);
  }
}

int StdlibDocsFunctionsOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

// ============================================================================
// StdlibDocsMacrosOperator
// ============================================================================

int StdlibDocsMacrosOperator::Connect(sqlite3* db,
                                      void* raw_ctx,
                                      int,
                                      const char* const*,
                                      sqlite3_vtab** vtab,
                                      char**) {
  if (int r = sqlite3_declare_vtab(db, kMacrosSchema); r != SQLITE_OK) {
    return r;
  }
  auto res = std::make_unique<Vtab>();
  res->engine = GetContext(raw_ctx);
  *vtab = res.release();
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::BestIndex(sqlite3_vtab*,
                                        sqlite3_index_info* info) {
  base::Status status = sqlite::utils::ValidateFunctionArguments(
      info, 1, [](size_t col) { return col == Column::kModuleArg; });
  if (!status.ok()) {
    return SQLITE_CONSTRAINT;
  }
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cur) {
  *cur = std::make_unique<Cursor>().release();
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::Close(sqlite3_vtab_cursor* cur) {
  delete GetCursor(cur);
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::Filter(sqlite3_vtab_cursor* cur,
                                     int,
                                     const char*,
                                     int argc,
                                     sqlite3_value** argv) {
  auto* c = GetCursor(cur);
  c->rows.clear();
  c->index = 0;
  PERFETTO_DCHECK(argc == 1);
  const char* text = ValueText(argv[0]);
  if (!text) {
    return SQLITE_OK;
  }
  std::string module_key = text;
  const std::string* sql = FindModuleSql(GetVtab(c->pVtab)->engine, module_key);
  if (!sql) {
    return sqlite::utils::SetError(
        GetVtab(c->pVtab),
        base::ErrStatus("Module not found: %s", module_key.c_str()));
  }
  auto parsed = stdlib_doc::ParseStdlibModule(
      sql->c_str(), static_cast<uint32_t>(sql->size()));
  for (const auto& error : parsed.errors) {
    PERFETTO_DLOG("perfetto_stdlib_macros: parse error in '%s': %s",
                  module_key.c_str(), error.c_str());
  }
  c->rows = std::move(parsed.macros);
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::Next(sqlite3_vtab_cursor* cur) {
  GetCursor(cur)->index++;
  return SQLITE_OK;
}

int StdlibDocsMacrosOperator::Eof(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  return c->index >= c->rows.size();
}

int StdlibDocsMacrosOperator::Column(sqlite3_vtab_cursor* cur,
                                     sqlite3_context* ctx,
                                     int col) {
  auto* c = GetCursor(cur);
  const auto& row = c->rows[c->index];
  switch (col) {
    case Column::kName:
      sqlite::result::StaticString(ctx, row.name.c_str());
      return SQLITE_OK;
    case Column::kDescription:
      sqlite::result::StaticString(ctx, row.description.c_str());
      return SQLITE_OK;
    case Column::kExposed:
      sqlite::result::Long(ctx, row.exposed ? 1 : 0);
      return SQLITE_OK;
    case Column::kReturnType:
      sqlite::result::StaticString(ctx, row.return_type.c_str());
      return SQLITE_OK;
    case Column::kReturnDescription:
      sqlite::result::StaticString(ctx, row.return_description.c_str());
      return SQLITE_OK;
    case Column::kArgs:
      sqlite::result::TransientString(ctx, SerializeEntries(row.args).c_str());
      return SQLITE_OK;
    case Column::kModuleArg:
      return SQLITE_OK;
    default:
      PERFETTO_FATAL("Unknown column %d", col);
  }
}

int StdlibDocsMacrosOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor
