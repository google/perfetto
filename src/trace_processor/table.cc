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

#include "perfetto/base/logging.h"
#include "src/trace_processor/query_constraints.h"

namespace perfetto {
namespace trace_processor {

namespace {

struct TableDescriptor {
  std::string name;
  std::string create_statement;
  Table::Factory factory;
  const TraceStorage* storage = nullptr;
  sqlite3_module module = {};
};

Table* ToTable(sqlite3_vtab* vtab) {
  return static_cast<Table*>(vtab);
}

Table::Cursor* ToCursor(sqlite3_vtab_cursor* cursor) {
  return static_cast<Table::Cursor*>(cursor);
}

}  // namespace

// static
bool Table::debug = false;

Table::Table() = default;
Table::~Table() = default;

void Table::RegisterInternal(sqlite3* db,
                             const TraceStorage* storage,
                             const std::string& create_statement,
                             Factory factory) {
  const std::string prefix = "CREATE TABLE ";
  PERFETTO_CHECK(create_statement.substr(0, prefix.size()) == prefix);
  size_t name_end = create_statement.find("(");
  PERFETTO_CHECK(name_end != std::string::npos && name_end > prefix.size());
  auto table_name =
      create_statement.substr(prefix.size(), name_end - prefix.size());
  PERFETTO_CHECK(table_name.size() > 0 && isalnum(table_name.front()) &&
                 isalnum(table_name.back()));

  std::unique_ptr<TableDescriptor> desc(new TableDescriptor());
  desc->name = table_name;
  desc->create_statement = create_statement;
  desc->storage = storage;
  desc->factory = factory;
  sqlite3_module* module = &desc->module;
  memset(module, 0, sizeof(*module));

  module->xConnect = [](sqlite3* xdb, void* arg, int, const char* const*,
                        sqlite3_vtab** tab, char**) {
    const TableDescriptor* xdesc = static_cast<const TableDescriptor*>(arg);
    int res = sqlite3_declare_vtab(xdb, xdesc->create_statement.c_str());
    if (res != SQLITE_OK)
      return res;

    // Freed in xDisconnect().
    *tab = xdesc->factory(xdesc->storage, xdesc->name).release();

    return SQLITE_OK;
  };

  module->xDisconnect = [](sqlite3_vtab* t) {
    delete ToTable(t);
    return SQLITE_OK;
  };

  module->xOpen = [](sqlite3_vtab* t, sqlite3_vtab_cursor** c) {
    return ToTable(t)->OpenInternal(c);
  };

  module->xClose = [](sqlite3_vtab_cursor* c) {
    delete ToCursor(c);
    return SQLITE_OK;
  };

  module->xBestIndex = [](sqlite3_vtab* t, sqlite3_index_info* i) {
    return ToTable(t)->BestIndexInternal(i);
  };

  module->xFilter = [](sqlite3_vtab_cursor* c, int i, const char* s, int a,
                       sqlite3_value** v) {
    return ToCursor(c)->FilterInternal(i, s, a, v);
  };
  module->xNext = [](sqlite3_vtab_cursor* c) { return ToCursor(c)->Next(); };
  module->xEof = [](sqlite3_vtab_cursor* c) { return ToCursor(c)->Eof(); };
  module->xColumn = [](sqlite3_vtab_cursor* c, sqlite3_context* a, int b) {
    return ToCursor(c)->Column(a, b);
  };

  module->xRowid = [](sqlite3_vtab_cursor*, sqlite_int64*) {
    return SQLITE_ERROR;
  };

  module->xFindFunction =
      [](sqlite3_vtab* t, int, const char* name,
         void (**fn)(sqlite3_context*, int, sqlite3_value**),
         void** args) { return ToTable(t)->FindFunction(name, fn, args); };

  int res = sqlite3_create_module_v2(
      db, table_name.c_str(), module, desc.release(),
      [](void* arg) { delete static_cast<TableDescriptor*>(arg); });
  PERFETTO_CHECK(res == SQLITE_OK);
}

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

  return SQLITE_OK;
}

int Table::FindFunction(const char*, FindFunctionFn, void**) {
  return 0;
};

Table::Cursor::~Cursor() = default;

int Table::Cursor::FilterInternal(int,
                                  const char* idxStr,
                                  int argc,
                                  sqlite3_value** argv) {
  if (Table::debug) {
    PERFETTO_LOG("[%s::Filter] constraints=%s argc=%d",
                 ToTable(this->pVtab)->name_.c_str(), idxStr, argc);
  }

  QueryConstraints qc = QueryConstraints::FromString(idxStr);
  PERFETTO_DCHECK(qc.constraints().size() == static_cast<size_t>(argc));
  return Filter(qc, argv);
}

}  // namespace trace_processor
}  // namespace perfetto
