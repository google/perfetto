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

#include "src/trace_processor/sqlite/stats_table.h"

#include <sqlite3.h>
#include <cstddef>
#include <memory>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/storage/stats.h"

namespace perfetto::trace_processor {

namespace {

// Advances the cursor to the next valid row within the current context.
// Returns true if a valid row was found, false if the context is exhausted.
bool AdvanceKeyInContext(StatsModule::Cursor* c) {
  if (!c->current_map)
    return false;

  const auto* cur_entry = &(*c->current_map)[c->key];
  if (stats::kTypes[c->key] == stats::kIndexed) {
    if (++c->it != cur_entry->indexed_values.end()) {
      return true;
    }
  }
  while (++c->key < stats::kNumKeys) {
    cur_entry = &(*c->current_map)[c->key];
    c->it = cur_entry->indexed_values.begin();
    if (stats::kTypes[c->key] == stats::kSingle ||
        !cur_entry->indexed_values.empty()) {
      return true;
    }
  }
  return false;
}

// Positions the cursor at the first valid row within the current context,
// starting from key 0. Returns true if a valid row was found.
bool InitContextCursor(StatsModule::Cursor* c) {
  if (c->context_idx >= c->contexts.size())
    return false;

  c->current_map = c->tracker->FindStatsForContext(c->contexts[c->context_idx]);
  if (!c->current_map)
    return false;

  static_assert(stats::kTypes[0] == stats::kSingle,
                "the first stats entry cannot be indexed");
  c->key = 0;
  return true;
}

}  // namespace

int StatsModule::Connect(sqlite3* db,
                         void* aux,
                         int,
                         const char* const*,
                         sqlite3_vtab** vtab,
                         char**) {
  static constexpr char kSchema[] = R"(
    CREATE TABLE x(
      name TEXT,
      idx BIGINT,
      severity TEXT,
      source TEXT,
      value BIGINT,
      description TEXT,
      machine_id BIGINT,
      trace_id BIGINT,
      key BIGINT HIDDEN,
      PRIMARY KEY(name, idx)
    ) WITHOUT ROWID
  )";
  if (int ret = sqlite3_declare_vtab(db, kSchema); ret != SQLITE_OK) {
    return ret;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->tracker = GetContext(aux);
  *vtab = res.release();
  return SQLITE_OK;
}

int StatsModule::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int StatsModule::BestIndex(sqlite3_vtab*, sqlite3_index_info*) {
  return SQLITE_OK;
}

int StatsModule::Open(sqlite3_vtab* raw_vtab, sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  c->tracker = GetVtab(raw_vtab)->tracker;
  *cursor = c.release();
  return SQLITE_OK;
}

int StatsModule::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int StatsModule::Filter(sqlite3_vtab_cursor* cursor,
                        int,
                        const char*,
                        int,
                        sqlite3_value**) {
  static_assert(stats::kTypes[0] == stats::kSingle,
                "the first stats entry cannot be indexed");

  auto* c = GetCursor(cursor);
  c->contexts = c->tracker->context_keys();
  c->context_idx = 0;
  c->key = 0;
  c->current_map = nullptr;

  // Position at first valid row.
  if (!InitContextCursor(c)) {
    // No contexts with data - mark as EOF.
    c->context_idx = c->contexts.size();
  }
  return SQLITE_OK;
}

int StatsModule::Next(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);

  // Try to advance within the current context.
  if (AdvanceKeyInContext(c)) {
    return SQLITE_OK;
  }

  // Current context exhausted, move to next context.
  while (++c->context_idx < c->contexts.size()) {
    if (InitContextCursor(c)) {
      return SQLITE_OK;
    }
  }

  // All contexts exhausted.
  return SQLITE_OK;
}

int StatsModule::Eof(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);
  return c->context_idx >= c->contexts.size();
}

int StatsModule::Column(sqlite3_vtab_cursor* cursor,
                        sqlite3_context* ctx,
                        int N) {
  auto* c = GetCursor(cursor);
  switch (N) {
    case Column::kName:
      sqlite::result::StaticString(ctx, stats::kNames[c->key]);
      break;
    case Column::kIndex:
      if (stats::kTypes[c->key] == stats::kIndexed) {
        sqlite::result::Long(ctx, c->it->first);
      } else {
        sqlite::result::Null(ctx);
      }
      break;
    case Column::kSeverity:
      switch (stats::kSeverities[c->key]) {
        case stats::kInfo:
          sqlite::result::StaticString(ctx, "info");
          break;
        case stats::kDataLoss:
          sqlite::result::StaticString(ctx, "data_loss");
          break;
        case stats::kError:
          sqlite::result::StaticString(ctx, "error");
          break;
      }
      break;
    case Column::kSource:
      switch (stats::kSources[c->key]) {
        case stats::kTrace:
          sqlite::result::StaticString(ctx, "trace");
          break;
        case stats::kAnalysis:
          sqlite::result::StaticString(ctx, "analysis");
          break;
      }
      break;
    case Column::kValue:
      if (stats::kTypes[c->key] == stats::kIndexed) {
        sqlite::result::Long(ctx, c->it->second);
      } else {
        sqlite::result::Long(ctx, (*c->current_map)[c->key].value);
      }
      break;
    case Column::kDescription:
      sqlite::result::StaticString(ctx, stats::kDescriptions[c->key]);
      break;
    case Column::kMachineId: {
      const auto& ctx_key = c->contexts[c->context_idx];
      if (ctx_key.machine_id.has_value()) {
        sqlite::result::Long(ctx, ctx_key.machine_id->value);
      } else {
        sqlite::result::Null(ctx);
      }
      break;
    }
    case Column::kTraceId: {
      const auto& ctx_key = c->contexts[c->context_idx];
      if (ctx_key.trace_id.has_value()) {
        sqlite::result::Long(ctx, ctx_key.trace_id->value);
      } else {
        sqlite::result::Null(ctx);
      }
      break;
    }
    case Column::kKey:
      sqlite::result::Long(ctx, static_cast<int64_t>(c->key));
      break;
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int StatsModule::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor
