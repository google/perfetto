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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

dataframe::Op SqliteOpToDataframeOp(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return dataframe::Eq();
    default:
      PERFETTO_FATAL("Unimplemented");
  }
}

// TODO(lalitm): take this from the sqlite context instead.
struct ConstantValueFetcher : dataframe::ValueFetcher {};

}  // namespace

int DataframeModule::Connect(sqlite3* db,
                             void*,
                             int,
                             const char* const*,
                             sqlite3_vtab** vtab,
                             char**) {
  static constexpr char kSchema[] = R"(
    CREATE TABLE x(
      id INTEGER NOT NULL,
      PRIMARY KEY(id)
    ) WITHOUT ROWID
  )";
  if (int ret = sqlite3_declare_vtab(db, kSchema); ret != SQLITE_OK) {
    return ret;
  }
  StringPool pool;
  // TODO(lalitm): actually create a dataframe properly and return it here.
  std::unique_ptr<Vtab> res;
  *vtab = res.release();
  return SQLITE_OK;
}

int DataframeModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  return SQLITE_OK;
}

int DataframeModule::BestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
  auto* v = GetVtab(tab);

  std::vector<dataframe::FilterSpec> filter_specs;
  filter_specs.reserve(static_cast<size_t>(info->nConstraint));
  for (int i = 0; i < info->nConstraint; ++i) {
    if (!info->aConstraint[i].usable) {
      continue;
    }
    filter_specs.emplace_back(dataframe::FilterSpec{
        static_cast<uint32_t>(info->aConstraint[i].iColumn),
        static_cast<uint32_t>(i),
        SqliteOpToDataframeOp(info->aConstraint[i].op),
        std::nullopt,
    });
  }
  SQLITE_ASSIGN_OR_RETURN(tab, auto plan,
                          v->dataframe.PlanQuery(filter_specs, info->colUsed));
  for (const auto& c : filter_specs) {
    if (auto value_index = c.value_index; value_index) {
      info->aConstraintUsage[c.source_index].argvIndex =
          static_cast<int>(*value_index) + 1;
      info->aConstraintUsage[c.source_index].omit = true;
    }
  }
  info->idxStr = sqlite3_mprintf("%s", std::move(plan).Serialize().data());
  info->needToFreeIdxStr = true;
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
    v->dataframe.PrepareCursor(plan, c->df_cursor);
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
