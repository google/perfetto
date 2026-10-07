/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/scatter_mipmap_operator/scatter_mipmap_operator.h"

#include <sqlite3.h>
#include <array>
#include <cmath>
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
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/module_state_manager.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor::scatter_mipmap_operator {
namespace {

constexpr char kSchema[] = R"(
  CREATE TABLE x(
    in_x_min DOUBLE HIDDEN,
    in_x_max DOUBLE HIDDEN,
    in_y_min DOUBLE HIDDEN,
    in_y_max DOUBLE HIDDEN,
    in_cols INTEGER HIDDEN,
    in_rows INTEGER HIDDEN,
    in_hidden TEXT HIDDEN,
    out_count INTEGER HIDDEN,
    out_x_min DOUBLE HIDDEN,
    out_x_max DOUBLE HIDDEN,
    out_y_min DOUBLE HIDDEN,
    out_y_max DOUBLE HIDDEN,
    out_c_min DOUBLE HIDDEN,
    out_c_max DOUBLE HIDDEN,
    out_cat_idx INTEGER HIDDEN,
    out_cat_value HIDDEN,
    out_cat_count INTEGER HIDDEN,
    id BIGINT,
    x DOUBLE,
    y DOUBLE,
    c DOUBLE,
    count INTEGER,
    PRIMARY KEY(id)
  ) WITHOUT ROWID
)";

enum ColumnIndex : size_t {
  kInXMin = 0,
  kInXMax,
  kInYMin,
  kInYMax,
  kInCols,
  kInRows,
  // Optional: comma-separated category indexes to exclude (-1 = NULL).
  kInHidden,

  kOutCount,
  kOutXMin,
  kOutXMax,
  kOutYMin,
  kOutYMax,
  kOutCMin,
  kOutCMax,
  kOutCatIdx,
  kOutCatValue,
  kOutCatCount,

  kId,
  kX,
  kY,
  kC,
  kCount,
};

constexpr size_t kRequiredArgCount = kInRows + 1;
constexpr size_t kArgCount = kInHidden + 1;

// BestIndex idxNum flag: the optional `in_hidden` argument is present.
constexpr int kHasHiddenArg = 1;

bool IsArgColumn(size_t index) {
  return index < kArgCount;
}

std::string TrimAndUnquote(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\n' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') ||
                        (s.front() == '"' && s.back() == '"'))) {
    s.remove_prefix(1);
    s.remove_suffix(1);
  }
  return std::string(s);
}

std::string QuoteIdentifier(std::string_view name) {
  std::string res = "\"";
  for (char ch : name) {
    if (ch == '"') {
      res += "\"\"";
    } else {
      res += ch;
    }
  }
  res += "\"";
  return res;
}

}  // namespace

int ScatterMipmapOperator::Create(sqlite3* db,
                                  void* raw_ctx,
                                  int argc,
                                  const char* const* argv,
                                  sqlite3_vtab** vtab,
                                  char** zErr) {
  if (argc != 4 && argc != 7 && argc != 8 && argc != 9) {
    *zErr = sqlite3_mprintf(
        "scatter_mipmap: expected 1 table/query arg, or 4-6 args (table, id, "
        "x, y, [c], [categories_top_n])");
    return SQLITE_ERROR;
  }

  if (int ret = sqlite3_declare_vtab(db, kSchema); ret != SQLITE_OK) {
    return ret;
  }

  auto* ctx = GetContext(raw_ctx);
  auto state = std::make_unique<State>();

  std::string table_name;
  std::string id_col = "id";
  std::string x_col = "x";
  std::string y_col = "y";
  std::string c_col;
  uint32_t auto_category_top_n = 0;
  bool is_table_mode = false;
  bool id_omitted = false;

  if (argc == 7 || argc == 8 || argc == 9) {
    table_name = TrimAndUnquote(argv[3]);
    id_col = TrimAndUnquote(argv[4]);
    if (id_col.empty() || id_col == "NULL" || id_col == "null") {
      id_omitted = true;
    }
    x_col = TrimAndUnquote(argv[5]);
    y_col = TrimAndUnquote(argv[6]);
    c_col = (argc >= 8) ? TrimAndUnquote(argv[7]) : "";
    if (c_col == "NULL" || c_col == "null") {
      c_col = "";
    }
    if (argc == 9) {
      std::string arg6 = TrimAndUnquote(argv[8]);
      if (c_col.empty()) {
        *zErr = sqlite3_mprintf(
            "scatter_mipmap: categories specified but color column 'c' is "
            "missing");
        return SQLITE_ERROR;
      }
      if (arg6.empty()) {
        *zErr = sqlite3_mprintf(
            "scatter_mipmap: categories argument cannot be empty");
        return SQLITE_ERROR;
      }
      std::optional<int32_t> top_n = base::StringToInt32(arg6);
      if (!top_n.has_value() || *top_n <= 0) {
        *zErr = sqlite3_mprintf(
            "scatter_mipmap: categories N must be positive, got: %s",
            arg6.c_str());
        return SQLITE_ERROR;
      }
      auto_category_top_n = static_cast<uint32_t>(*top_n);
    }
    is_table_mode = true;
  } else {
    std::string arg = TrimAndUnquote(argv[3]);
    if (!arg.empty() && arg.front() != '(') {
      table_name = arg;
      id_col = "id";
      x_col = "x";
      y_col = "y";
      c_col = "c";
      is_table_mode = true;
    }
  }

  CategoryMapping cat_mapping;
  if (auto_category_top_n > 0) {
    cat_mapping.auto_top_n = auto_category_top_n;
  }

  bool ingested_from_dataframe = false;

  if (is_table_mode) {
    const auto* df = ctx->connection->GetDataframeOrNull(table_name);
    if (df) {
      const auto& col_names = df->column_names();
      int id_idx = -1, x_idx = -1, y_idx = -1, c_idx = -1;
      for (uint32_t i = 0; i < col_names.size(); ++i) {
        if (!id_omitted && col_names[i] == id_col)
          id_idx = static_cast<int>(i);
        if (col_names[i] == x_col)
          x_idx = static_cast<int>(i);
        if (col_names[i] == y_col)
          y_idx = static_cast<int>(i);
        if (!c_col.empty() && col_names[i] == c_col)
          c_idx = static_cast<int>(i);
      }
      if (!id_omitted && id_idx < 0) {
        *zErr = sqlite3_mprintf(
            "scatter_mipmap: column '%s' does not exist in table '%s'",
            id_col.c_str(), table_name.c_str());
        return SQLITE_ERROR;
      }
      if (x_idx < 0) {
        *zErr = sqlite3_mprintf(
            "scatter_mipmap: column '%s' does not exist in table '%s'",
            x_col.c_str(), table_name.c_str());
        return SQLITE_ERROR;
      }
      if (y_idx < 0) {
        *zErr = sqlite3_mprintf(
            "scatter_mipmap: column '%s' does not exist in table '%s'",
            y_col.c_str(), table_name.c_str());
        return SQLITE_ERROR;
      }
      if (!c_col.empty() && c_idx < 0) {
        if (argc == 4) {
          // For bare name form, c is optional.
          c_col = "";
        } else {
          *zErr = sqlite3_mprintf(
              "scatter_mipmap: column '%s' does not exist in table '%s'",
              c_col.c_str(), table_name.c_str());
          return SQLITE_ERROR;
        }
      }

      DataframeColumnReader reader_id, reader_x, reader_y, reader_c;
      bool id_ok = id_omitted ||
                   reader_id.Init(df->column(static_cast<uint32_t>(id_idx)));
      if (id_ok && reader_x.Init(df->column(static_cast<uint32_t>(x_idx))) &&
          reader_y.Init(df->column(static_cast<uint32_t>(y_idx))) &&
          (c_idx < 0 ||
           reader_c.Init(df->column(static_cast<uint32_t>(c_idx))))) {
        if (!id_omitted && !reader_id.is_integer()) {
          *zErr = sqlite3_mprintf(
              "scatter_mipmap: id column '%s' must be an integer",
              id_col.c_str());
          return SQLITE_ERROR;
        }
        if (!reader_x.is_numeric()) {
          *zErr = sqlite3_mprintf(
              "scatter_mipmap: x column '%s' must be numeric", x_col.c_str());
          return SQLITE_ERROR;
        }
        if (!reader_y.is_numeric()) {
          *zErr = sqlite3_mprintf(
              "scatter_mipmap: y column '%s' must be numeric", y_col.c_str());
          return SQLITE_ERROR;
        }
        if (auto_category_top_n > 0) {
          if (reader_c.kind == DataframeColumnReader::kString) {
            cat_mapping.type = CategoryMapping::Type::kString;
          } else if (reader_c.kind == DataframeColumnReader::kDouble) {
            cat_mapping.type = CategoryMapping::Type::kDouble;
          } else if (reader_c.is_integer()) {
            cat_mapping.type = CategoryMapping::Type::kInt;
          } else {
            *zErr = sqlite3_mprintf(
                "scatter_mipmap: unsupported category column type");
            return SQLITE_ERROR;
          }
        } else if (c_idx >= 0 && !reader_c.is_numeric()) {
          *zErr = sqlite3_mprintf(
              "scatter_mipmap: color column '%s' must be numeric when "
              "categories are not used",
              c_col.c_str());
          return SQLITE_ERROR;
        }

        state->index.IngestAndBuildDataframe(
            df->row_count(), (id_omitted ? nullptr : &reader_id), reader_x,
            reader_y, (c_idx >= 0 ? &reader_c : nullptr),
            (auto_category_top_n > 0 ? &cat_mapping : nullptr),
            ctx->connection->string_pool());
        ingested_from_dataframe = true;
      }
    }
  }

  if (!ingested_from_dataframe) {
    std::string sql;
    if (is_table_mode) {
      sql = "SELECT ";
      if (id_omitted) {
        sql += "NULL AS id, ";
      } else {
        sql += QuoteIdentifier(id_col) + " AS id, ";
      }
      sql += QuoteIdentifier(x_col) + " AS x, ";
      sql += QuoteIdentifier(y_col) + " AS y, ";
      if (c_col.empty()) {
        sql += "NULL AS c ";
      } else {
        sql += QuoteIdentifier(c_col) + " AS c ";
      }
      sql += "FROM " + QuoteIdentifier(table_name);
    } else {
      sql = "SELECT * FROM ";
      sql.append(argv[3]);
    }

    auto res = ctx->connection->ExecuteUntilLastStatement(
        SqlSource::FromTraceProcessorImplementation(std::move(sql)));
    if (!res.ok()) {
      *zErr = sqlite3_mprintf("%s", res.status().c_message());
      return SQLITE_ERROR;
    }

    sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
    int num_cols = sqlite3_column_count(stmt);
    if (!is_table_mode && (num_cols < 2 || num_cols > 4)) {
      *zErr = sqlite3_mprintf(
          "scatter_mipmap: query must return between 2 and 4 columns, got %d",
          num_cols);
      return SQLITE_ERROR;
    }

    int col_id_idx = -1;
    int col_x_idx = 0;
    int col_y_idx = 1;
    int col_c_idx = -1;

    if (is_table_mode || num_cols == 4) {
      col_id_idx = 0;
      col_x_idx = 1;
      col_y_idx = 2;
      col_c_idx = 3;
    } else if (num_cols == 3) {
      col_id_idx = -1;
      col_x_idx = 0;
      col_y_idx = 1;
      col_c_idx = 2;
    } else {
      col_id_idx = -1;
      col_x_idx = 0;
      col_y_idx = 1;
      col_c_idx = -1;
    }

    std::vector<SqlRow> rows;
    StringPool* pool = ctx->connection->string_pool();
    bool type_determined = false;
    int64_t ordinal = 0;

    for (bool more = !res->stmt.IsDone(); more; more = res->stmt.Step()) {
      int t_x = sqlite3_column_type(stmt, col_x_idx);
      int t_y = sqlite3_column_type(stmt, col_y_idx);
      if (t_x == SQLITE_NULL || t_y == SQLITE_NULL) {
        ordinal++;
        continue;
      }
      if (t_x == SQLITE_TEXT || t_y == SQLITE_TEXT || t_x == SQLITE_BLOB ||
          t_y == SQLITE_BLOB) {
        *zErr =
            sqlite3_mprintf("scatter_mipmap: x and y columns must be numeric");
        return SQLITE_ERROR;
      }
      double x = sqlite3_column_double(stmt, col_x_idx);
      double y = sqlite3_column_double(stmt, col_y_idx);
      if (!std::isfinite(x) || !std::isfinite(y)) {
        ordinal++;
        continue;
      }

      int64_t id = ordinal;
      if (col_id_idx >= 0) {
        int t_id = sqlite3_column_type(stmt, col_id_idx);
        if (t_id != SQLITE_NULL) {
          if (t_id != SQLITE_INTEGER) {
            *zErr =
                sqlite3_mprintf("scatter_mipmap: id column must be an integer");
            return SQLITE_ERROR;
          }
          id = sqlite3_column_int64(stmt, col_id_idx);
        }
      }
      ordinal++;

      SqlRow row;
      row.id = id;
      row.x = x;
      row.y = y;

      if (col_c_idx >= 0) {
        int t_c = sqlite3_column_type(stmt, col_c_idx);
        if (t_c != SQLITE_NULL) {
          row.has_c = true;
          row.c_is_null = false;
          if (auto_category_top_n > 0) {
            if (t_c == SQLITE_TEXT) {
              const char* txt = reinterpret_cast<const char*>(
                  sqlite3_column_text(stmt, col_c_idx));
              StringPool::Id s_id = pool->InternString(base::StringView(txt));
              row.c_is_string = true;
              row.c_str_id = s_id.raw_id();
              if (!type_determined) {
                cat_mapping.type = CategoryMapping::Type::kString;
                type_determined = true;
              }
            } else if (t_c == SQLITE_INTEGER) {
              row.c_is_int = true;
              row.c_int = sqlite3_column_int64(stmt, col_c_idx);
              if (!type_determined) {
                cat_mapping.type = CategoryMapping::Type::kInt;
                type_determined = true;
              }
            } else if (t_c == SQLITE_FLOAT) {
              row.c_is_double = true;
              row.c_double = sqlite3_column_double(stmt, col_c_idx);
              if (!type_determined) {
                cat_mapping.type = CategoryMapping::Type::kDouble;
                type_determined = true;
              }
            }
          } else {
            if (t_c == SQLITE_INTEGER || t_c == SQLITE_FLOAT) {
              row.c = sqlite3_column_double(stmt, col_c_idx);
            } else {
              row.c = std::numeric_limits<double>::quiet_NaN();
            }
          }
        }
      }
      rows.push_back(std::move(row));
    }

    if (!res->stmt.status().ok()) {
      *zErr = sqlite3_mprintf("%s", res->stmt.status().c_message());
      return SQLITE_ERROR;
    }

    state->index.IngestAndBuildSqlRows(
        rows, (auto_category_top_n > 0 ? &cat_mapping : nullptr), pool);
  }

  std::unique_ptr<Vtab> vtab_res = std::make_unique<Vtab>();
  vtab_res->state = ctx->OnCreate(argc, argv, std::move(state));
  *vtab = vtab_res.release();
  return SQLITE_OK;
}

int ScatterMipmapOperator::Destroy(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  sqlite::ModuleStateManager<ScatterMipmapOperator>::OnDestroy(tab->state);
  return SQLITE_OK;
}

int ScatterMipmapOperator::Connect(sqlite3* db,
                                   void* raw_ctx,
                                   int argc,
                                   const char* const* argv,
                                   sqlite3_vtab** vtab,
                                   char**) {
  if (argc != 4 && argc != 7 && argc != 8 && argc != 9) {
    return SQLITE_ERROR;
  }
  if (int ret = sqlite3_declare_vtab(db, kSchema); ret != SQLITE_OK) {
    return ret;
  }
  auto* ctx = GetContext(raw_ctx);
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->state = ctx->OnConnect(argc, argv);
  *vtab = res.release();
  return SQLITE_OK;
}

int ScatterMipmapOperator::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int ScatterMipmapOperator::BestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
  std::array<bool, kArgCount> seen_args{};
  size_t seen_count = 0;

  for (int i = 0; i < info->nConstraint; ++i) {
    auto& usage = info->aConstraintUsage[i];
    usage.argvIndex = 0;
    usage.omit = false;

    const auto& c = info->aConstraint[i];
    if (!c.usable) {
      continue;
    }
    if (c.op == SQLITE_INDEX_CONSTRAINT_LIMIT ||
        c.op == SQLITE_INDEX_CONSTRAINT_OFFSET) {
      continue;
    }
    if (c.iColumn < 0) {
      continue;
    }
    size_t col = static_cast<size_t>(c.iColumn);
    if (!IsArgColumn(col)) {
      continue;
    }
    if (c.op != SQLITE_INDEX_CONSTRAINT_EQ) {
      continue;
    }
    if (seen_args[col]) {
      return SQLITE_CONSTRAINT;
    }
    seen_args[col] = true;
    ++seen_count;
    usage.argvIndex = static_cast<int>(col + 1);
    usage.omit = true;
  }

  for (size_t i = 0; i < kRequiredArgCount; ++i) {
    if (!seen_args[i]) {
      return SQLITE_CONSTRAINT;
    }
  }
  PERFETTO_DCHECK(seen_count == kRequiredArgCount || seen_count == kArgCount);
  info->idxNum = seen_args[kInHidden] ? kHasHiddenArg : 0;

  info->estimatedCost = 100.0;
  info->estimatedRows = 1000;
  return SQLITE_OK;
}

int ScatterMipmapOperator::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int ScatterMipmapOperator::Close(sqlite3_vtab_cursor* cursor) {
  std::unique_ptr<Cursor> c(GetCursor(cursor));
  return SQLITE_OK;
}

int ScatterMipmapOperator::Filter(sqlite3_vtab_cursor* cursor,
                                  int idx_num,
                                  const char*,
                                  int argc,
                                  sqlite3_value** argv) {
  auto* c = GetCursor(cursor);
  auto* t = GetVtab(c->pVtab);
  auto* state =
      sqlite::ModuleStateManager<ScatterMipmapOperator>::GetState(t->state);
  const bool has_hidden_arg = (idx_num & kHasHiddenArg) != 0;
  PERFETTO_CHECK(
      argc == static_cast<int>(has_hidden_arg ? kArgCount : kRequiredArgCount));

  c->results.clear();
  c->current_index = 0;
  c->is_listing_mode = false;

  int64_t cols = sqlite3_value_int64(argv[4]);
  int64_t rows = sqlite3_value_int64(argv[5]);

  if (cols == 0 && rows == 0) {
    c->is_listing_mode = true;
    return SQLITE_OK;
  }

  if (state->index.empty()) {
    return SQLITE_OK;
  }

  bool x_min_null = (sqlite3_value_type(argv[0]) == SQLITE_NULL);
  bool x_max_null = (sqlite3_value_type(argv[1]) == SQLITE_NULL);
  bool y_min_null = (sqlite3_value_type(argv[2]) == SQLITE_NULL);
  bool y_max_null = (sqlite3_value_type(argv[3]) == SQLITE_NULL);

  double x_min =
      x_min_null ? state->index.x_min_data() : sqlite3_value_double(argv[0]);
  double x_max =
      x_max_null ? state->index.x_max_data() : sqlite3_value_double(argv[1]);
  double y_min =
      y_min_null ? state->index.y_min_data() : sqlite3_value_double(argv[2]);
  double y_max =
      y_max_null ? state->index.y_max_data() : sqlite3_value_double(argv[3]);

  if (!std::isfinite(x_min) || !std::isfinite(x_max) || !std::isfinite(y_min) ||
      !std::isfinite(y_max)) {
    return sqlite::utils::SetError(t, "scatter_mipmap: non-finite bounds");
  }
  if (x_max_null && x_max <= x_min) {
    x_min -= 0.5;
    x_max += 0.5;
  }
  if (y_max_null && y_max <= y_min) {
    y_min -= 0.5;
    y_max += 0.5;
  }
  if (x_max <= x_min) {
    return sqlite::utils::SetError(
        t, "scatter_mipmap: x_max must be greater than x_min");
  }
  if (y_max <= y_min) {
    return sqlite::utils::SetError(
        t, "scatter_mipmap: y_max must be greater than y_min");
  }
  constexpr int64_t kMaxCells = 16'000'000;
  if (cols < 1 || rows < 1) {
    return sqlite::utils::SetError(
        t, "scatter_mipmap: cols and rows must be at least 1");
  }
  if (cols > kMaxCells || rows > kMaxCells || cols > kMaxCells / rows) {
    return sqlite::utils::SetError(
        t, "scatter_mipmap: cols * rows must not exceed 16M");
  }

  ScatterMipmapIndex::CategoryFilter filter;
  bool has_filter = false;
  if (has_hidden_arg && sqlite3_value_type(argv[kInHidden]) != SQLITE_NULL) {
    const char* text =
        reinterpret_cast<const char*>(sqlite3_value_text(argv[kInHidden]));
    for (const std::string& token :
         base::SplitString(text != nullptr ? text : "", ",")) {
      std::string trimmed = base::TrimWhitespace(token);
      if (trimmed.empty()) {
        continue;
      }
      std::optional<int32_t> idx = base::StringToInt32(trimmed);
      if (!idx.has_value() || *idx < -1) {
        return sqlite::utils::SetError(
            t,
            "scatter_mipmap: hidden must be a comma-separated list of "
            "category indexes >= -1");
      }
      if (*idx == -1) {
        filter.hide_null = true;
      } else {
        auto k = static_cast<size_t>(*idx);
        if (k > state->index.categories().size()) {
          // No such category (indexes never exceed top-N, i.e. "Other").
          continue;
        }
        if (k >= filter.hidden.size()) {
          filter.hidden.resize(k + 1, false);
        }
        filter.hidden[k] = true;
      }
      has_filter = true;
    }
  }

  state->index.Query(x_min, x_max, y_min, y_max, cols, rows, c->buffers,
                     c->results, has_filter ? &filter : nullptr);
  return SQLITE_OK;
}

int ScatterMipmapOperator::Next(sqlite3_vtab_cursor* cursor) {
  GetCursor(cursor)->current_index++;
  return SQLITE_OK;
}

int ScatterMipmapOperator::Eof(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);
  auto* t = GetVtab(c->pVtab);
  auto* state =
      sqlite::ModuleStateManager<ScatterMipmapOperator>::GetState(t->state);
  if (c->is_listing_mode) {
    return c->current_index >= state->index.categories().size();
  }
  return c->current_index >= c->results.size();
}

int ScatterMipmapOperator::Column(sqlite3_vtab_cursor* cursor,
                                  sqlite3_context* ctx,
                                  int N) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);
  auto* state =
      sqlite::ModuleStateManager<ScatterMipmapOperator>::GetState(t->state);

  if (c->is_listing_mode) {
    if (c->current_index >= state->index.categories().size()) {
      sqlite::result::Null(ctx);
      return SQLITE_OK;
    }
    const auto& cat = state->index.categories()[c->current_index];
    switch (N) {
      case ColumnIndex::kOutCatIdx:
        sqlite::result::Long(ctx, cat.idx);
        return SQLITE_OK;
      case ColumnIndex::kOutCatValue:
        if (cat.is_null) {
          sqlite::result::Null(ctx);
        } else if (cat.is_other) {
          sqlite::result::StaticString(ctx, "Other");
        } else if (state->index.categories_type() ==
                   CategoryMapping::Type::kString) {
          sqlite::result::TransientString(ctx, cat.str_val.c_str());
        } else if (state->index.categories_type() ==
                   CategoryMapping::Type::kInt) {
          sqlite::result::Long(ctx, cat.int_val);
        } else {
          sqlite::result::Double(ctx, cat.double_val);
        }
        return SQLITE_OK;
      case ColumnIndex::kOutCatCount:
        sqlite::result::Long(ctx, static_cast<int64_t>(cat.count));
        return SQLITE_OK;
      case ColumnIndex::kId:
        sqlite::result::Long(ctx, static_cast<int64_t>(c->current_index));
        return SQLITE_OK;
      default:
        sqlite::result::Null(ctx);
        return SQLITE_OK;
    }
  }

  const auto& res = c->results[c->current_index];
  uint32_t rep_idx = res.rep_index;

  switch (N) {
    case ColumnIndex::kInXMin:
    case ColumnIndex::kInXMax:
    case ColumnIndex::kInYMin:
    case ColumnIndex::kInYMax:
    case ColumnIndex::kInCols:
    case ColumnIndex::kInRows:
    case ColumnIndex::kInHidden:
    case ColumnIndex::kOutCatIdx:
    case ColumnIndex::kOutCatValue:
    case ColumnIndex::kOutCatCount:
      sqlite::result::Null(ctx);
      return SQLITE_OK;
    case ColumnIndex::kOutCount:
      sqlite::result::Long(ctx, static_cast<int64_t>(state->index.size()));
      return SQLITE_OK;
    case ColumnIndex::kOutXMin:
      sqlite::result::Double(ctx, state->index.x_min_data());
      return SQLITE_OK;
    case ColumnIndex::kOutXMax:
      sqlite::result::Double(ctx, state->index.x_max_data());
      return SQLITE_OK;
    case ColumnIndex::kOutYMin:
      sqlite::result::Double(ctx, state->index.y_min_data());
      return SQLITE_OK;
    case ColumnIndex::kOutYMax:
      sqlite::result::Double(ctx, state->index.y_max_data());
      return SQLITE_OK;
    case ColumnIndex::kOutCMin:
      if (state->index.has_c()) {
        sqlite::result::Double(ctx, state->index.c_min_data());
      } else {
        sqlite::result::Null(ctx);
      }
      return SQLITE_OK;
    case ColumnIndex::kOutCMax:
      if (state->index.has_c()) {
        sqlite::result::Double(ctx, state->index.c_max_data());
      } else {
        sqlite::result::Null(ctx);
      }
      return SQLITE_OK;
    case ColumnIndex::kId:
      sqlite::result::Long(ctx, state->index.id(rep_idx));
      return SQLITE_OK;
    case ColumnIndex::kX:
      sqlite::result::Double(ctx, state->index.x(rep_idx));
      return SQLITE_OK;
    case ColumnIndex::kY:
      sqlite::result::Double(ctx, state->index.y(rep_idx));
      return SQLITE_OK;
    case ColumnIndex::kC:
      if (std::isnan(state->index.c(rep_idx))) {
        sqlite::result::Null(ctx);
      } else {
        sqlite::result::Double(ctx, state->index.c(rep_idx));
      }
      return SQLITE_OK;
    case ColumnIndex::kCount:
      sqlite::result::Long(ctx, static_cast<int64_t>(res.count));
      return SQLITE_OK;
    default:
      return sqlite::utils::SetError(t, "Bad column");
  }
  PERFETTO_FATAL("For GCC");
}

int ScatterMipmapOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

namespace {

class ScatterMipmapOperatorPlugin : public Plugin<ScatterMipmapOperatorPlugin> {
 public:
  ~ScatterMipmapOperatorPlugin() override;

  void RegisterSqliteModules(
      PerfettoSqlConnection* connection,
      std::vector<SqliteModuleRegistration>& out) override {
    out.push_back(MakeSqliteModule<ScatterMipmapOperator>(
        "__intrinsic_scatter_mipmap",
        std::make_unique<ScatterMipmapOperator::Context>(connection)));
  }
};

ScatterMipmapOperatorPlugin::~ScatterMipmapOperatorPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<ScatterMipmapOperatorPlugin>();
      },
      ScatterMipmapOperatorPlugin::kPluginId,
      ScatterMipmapOperatorPlugin::kDepIds.data(),
      ScatterMipmapOperatorPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::scatter_mipmap_operator
