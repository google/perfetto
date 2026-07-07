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

#include "src/trace_processor/duckdb/sql_lowering.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// A no-op macro resolver: run-path SQL is already macro-expanded, so any
// residual `name!(...)` is unknown. Returning NOT_FOUND makes such a statement
// fail to parse, and LowerSqlForDuckDb then returns the SQL untouched.
int NoMacros(void*,
             SyntaqliteParser*,
             const char*,
             SyntaqliteLength,
             const SyntaqliteToken*,
             uint32_t) {
  return SYNTAQLITE_MACRO_LOOKUP_NOT_FOUND;
}

// The macro-expanded text of `span` (statement-relative span -> bytes).
std::string SpanText(SyntaqliteParser* p, const SyntaqliteTextSpan& span) {
  uint32_t len = 0;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  return text ? std::string(text, len) : std::string();
}

// True if the whole query contains a clause that desyncs row_number() from the
// dataframe row index, mirroring RewriteAutoId's coarse guard exactly so the
// _auto_id rewrite reproduces the old behaviour byte-for-byte.
bool AutoIdUnsafe(const std::string& sql) {
  std::string low = base::ToLower(sql);
  for (const char* kw : {" join ", " where ", " group ", " having ", " union ",
                         " distinct ", " order by "}) {
    if (low.find(kw) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string ApplyEdits(const std::string& src, std::vector<SpanEdit> edits) {
  if (edits.empty()) {
    return src;
  }
  std::stable_sort(
      edits.begin(), edits.end(),
      [](const SpanEdit& a, const SpanEdit& b) { return a.offset < b.offset; });
  std::string out;
  out.reserve(src.size() + 16);
  uint32_t cursor = 0;
  for (const SpanEdit& e : edits) {
    // Drop any edit that overlaps already-emitted bytes (fail-safe; for the
    // structurally-derived edits below this should never trigger).
    if (e.offset < cursor) {
      continue;
    }
    out.append(src, cursor, e.offset - cursor);
    out.append(e.repl);
    cursor = e.offset + e.len;
  }
  out.append(src, cursor, src.size() - cursor);
  return out;
}

std::string LowerSqlForDuckDb(const std::string& sql) {
  SyntaqliteParser* p = syntaqlite_parser_create_with_dialect(
      nullptr, syntaqlite_perfetto_dialect());
  if (!p) {
    return sql;
  }
  if (syntaqlite_parser_set_collect_node_extents(p, 1) != 0) {
    syntaqlite_parser_destroy(p);
    return sql;
  }
  syntaqlite_parser_set_macro_lookup(p, &NoMacros, nullptr);
  syntaqlite_parser_reset(p, sql.data(), static_cast<uint32_t>(sql.size()));

  const bool auto_id_unsafe = AutoIdUnsafe(sql);

  std::vector<SpanEdit> edits;
  for (;;) {
    int32_t rc = syntaqlite_parser_next(p);
    if (rc == SYNTAQLITE_PARSE_DONE) {
      break;
    }
    if (rc == SYNTAQLITE_PARSE_ERROR) {
      // A statement we don't fully understand: abandon ALL edits and let the
      // original SQL flow to DuckDB unchanged (it errors -> SQLite fallback).
      syntaqlite_parser_destroy(p);
      return sql;
    }
    uint32_t root = syntaqlite_result_root(p);
    if (!syntaqlite_node_is_present(root)) {
      continue;  // Bare comments/whitespace between statements.
    }

    // Every span/extent the parser emits for this statement is measured from
    // this document-absolute offset within `sql`.
    uint32_t stmt_off = 0;
    syntaqlite_parser_text(p, &stmt_off, nullptr);

    // A non-backtracking LALR parse only allocates nodes that are part of the
    // final tree, so a linear arena scan visits exactly the live nodes.
    uint32_t node_count = syntaqlite_parser_node_count(p);
    for (uint32_t id = 0; id < node_count; ++id) {
      const auto* node =
          static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, id));
      if (!node) {
        continue;
      }
      switch (static_cast<uint32_t>(node->tag)) {
        case SYNTAQLITE_NODE_FUNCTION_CALL: {
          const SyntaqliteFunctionCall& fc = node->function_call;
          std::string name = base::ToLower(SpanText(p, fc.func_name));
          if (name == "format") {
            // SQLite format()/printf() are both C-style; DuckDB's printf() is
            // the C-style one (DuckDB format() is Python `{}`-style).
            edits.push_back(SpanEdit{stmt_off + fc.func_name.offset,
                                     fc.func_name.length, "printf"});
          } else if (name == "char") {
            // DuckDB has no char(); chr() takes ONE 32-bit INTEGER codepoint.
            // Only the single-argument form maps (SQLite char() is variadic).
            if (!syntaqlite_node_is_present(fc.args)) {
              break;
            }
            const auto* args = static_cast<const SyntaqliteExprList*>(
                syntaqlite_parser_node(p, fc.args));
            if (!args || args->count != 1) {
              break;
            }
            uint32_t arg_id = args->children[0];
            uint32_t arg_len = 0;
            uint32_t arg_off = 0;
            const char* arg_text =
                syntaqlite_parser_node_text(p, arg_id, &arg_len, &arg_off);
            if (!arg_text) {
              break;
            }
            edits.push_back(SpanEdit{stmt_off + fc.func_name.offset,
                                     fc.func_name.length, "chr"});
            edits.push_back(SpanEdit{stmt_off + arg_off, 0, "CAST("});
            edits.push_back(
                SpanEdit{stmt_off + arg_off + arg_len, 0, " AS INTEGER)"});
          }
          break;
        }
        case SYNTAQLITE_NODE_COLUMN_REF: {
          if (auto_id_unsafe) {
            break;
          }
          const SyntaqliteColumnRef& cr = node->column_ref;
          // Only an UNQUALIFIED, unquoted `_auto_id` (a qualified `t._auto_id`
          // names a specific table the row_number() rewrite can't reproduce).
          if (cr.table.length != 0 || cr.schema.length != 0 ||
              syntaqlite_span_is_quoted(cr.column)) {
            break;
          }
          if (base::ToLower(SpanText(p, cr.column)) == "_auto_id") {
            edits.push_back(SpanEdit{stmt_off + cr.column.offset,
                                     cr.column.length,
                                     "(row_number() OVER () - 1)"});
          }
          break;
        }
        default:
          break;
      }
    }
  }

  syntaqlite_parser_destroy(p);
  return ApplyEdits(sql, std::move(edits));
}

}  // namespace perfetto::trace_processor::duckdb_integration
