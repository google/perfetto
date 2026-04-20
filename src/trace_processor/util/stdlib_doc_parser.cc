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

#include "src/trace_processor/util/stdlib_doc_parser.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::trace_processor::stdlib_doc {

namespace {

// Returns the text for a SyntaqliteTextSpan as a std::string.
std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  uint32_t len;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  if (!text) {
    return {};
  }
  return {text, len};
}

// Strips leading "-- " or "--" from a single comment line.
std::string StripCommentPrefix(const std::string& line) {
  size_t pos = 0;
  // Skip leading whitespace.
  while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
    pos++;
  }
  if (pos + 1 < line.size() && line[pos] == '-' && line[pos + 1] == '-') {
    pos += 2;
    // Skip one optional space after --.
    if (pos < line.size() && line[pos] == ' ') {
      pos++;
    }
  }
  return line.substr(pos);
}

// Given the full SQL source and a byte offset of a statement, extracts the
// contiguous block of line comments (-- ...) immediately preceding the
// statement. Returns the joined description text.
std::string ExtractDescriptionAbove(const char* sql, uint32_t stmt_offset) {
  // Walk backwards from stmt_offset to find comment lines.
  // First skip any whitespace before the statement.
  int pos = static_cast<int>(stmt_offset) - 1;
  while (pos >= 0 && (sql[pos] == ' ' || sql[pos] == '\t' || sql[pos] == '\n' ||
                      sql[pos] == '\r')) {
    pos--;
  }

  // Now collect comment lines going backwards.
  std::vector<std::string> lines;
  while (pos >= 0) {
    // Find the start of the current line.
    int line_end = pos;
    while (pos >= 0 && sql[pos] != '\n') {
      pos--;
    }
    int line_start = pos + 1;

    std::string line(sql + line_start,
                     static_cast<size_t>(line_end - line_start + 1));

    // Trim trailing whitespace.
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                             line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }

    // Trim leading whitespace for the check.
    size_t first_non_ws = 0;
    while (first_non_ws < line.size() &&
           (line[first_non_ws] == ' ' || line[first_non_ws] == '\t')) {
      first_non_ws++;
    }

    // Check if this line is a comment.
    if (first_non_ws + 1 < line.size() && line[first_non_ws] == '-' &&
        line[first_non_ws + 1] == '-') {
      lines.push_back(StripCommentPrefix(line));
    } else {
      break;
    }
    // Move to the previous line.
    pos--;
  }

  // Reverse since we collected bottom-up.
  std::reverse(lines.begin(), lines.end());

  // Join lines.
  std::string result;
  for (size_t i = 0; i < lines.size(); i++) {
    if (i > 0) {
      result += ' ';
    }
    result += lines[i];
  }
  return result;
}

// Given the full SQL source and the byte range of a column/arg definition,
// extracts the inline comment (-- ...) immediately above it.
//
// |stmt_start_abs| is the document-absolute byte offset of the statement that
// owns these comments. SyntaqliteComment.offset is statement-relative, so we
// add stmt_start_abs to convert it to document-absolute before comparing
// against node_offset (which is already document-absolute).
std::string ExtractInlineComment(const char* sql,
                                 const SyntaqliteComment* comments,
                                 uint32_t comment_count,
                                 uint32_t stmt_start_abs,
                                 uint32_t node_offset) {
  // Find the comment that ends just before node_offset (on the line above).
  // We look for the last line comment whose end is closest to node_offset.
  const SyntaqliteComment* best = nullptr;
  for (uint32_t i = 0; i < comment_count; i++) {
    const auto& c = comments[i];
    // Only line comments (kind == 0).
    if (c.kind != 0) {
      continue;
    }
    // SyntaqliteComment.offset is statement-relative; convert to absolute.
    uint32_t abs_comment_start = stmt_start_abs + c.offset;
    uint32_t abs_comment_end = abs_comment_start + c.length;
    // The comment must end before the node starts.
    if (abs_comment_end > node_offset) {
      continue;
    }
    // Check there's only whitespace between comment end and node offset.
    bool only_ws = true;
    for (uint32_t j = abs_comment_end; j < node_offset; j++) {
      if (sql[j] != ' ' && sql[j] != '\t' && sql[j] != '\n' && sql[j] != '\r') {
        only_ws = false;
        break;
      }
    }
    if (only_ws) {
      if (!best || abs_comment_start > stmt_start_abs + best->offset) {
        best = &c;
      }
    }
  }
  if (!best) {
    return {};
  }
  std::string comment(sql + stmt_start_abs + best->offset, best->length);
  return StripCommentPrefix(comment);
}

// Extracts the return description — the contiguous block of line comments
// immediately preceding the RETURNS keyword. Scans back to the start of the
// RETURNS line so that ExtractDescriptionAbove stops at that keyword.
std::string ExtractReturnDescription(const char* sql,
                                     SyntaqliteParser* p,
                                     const SyntaqlitePerfettoReturnType* rt) {
  // All syntaqlite span/node offsets are statement-relative (measured from
  // syntaqlite_parser_text()'s returned pointer). The returned text pointer
  // itself is correct; use pointer arithmetic to get a document-absolute
  // offset.
  uint32_t type_len = 0;
  const char* type_ptr = nullptr;
  if (rt->kind == SYNTAQLITE_PERFETTO_RETURN_KIND_SCALAR) {
    type_ptr =
        syntaqlite_parser_span_text(p, &rt->scalar_type, &type_len, nullptr);
  } else {
    type_ptr =
        syntaqlite_parser_node_text(p, rt->table_columns, &type_len, nullptr);
  }
  if (!type_ptr || type_len == 0) {
    return {};
  }
  uint32_t type_offset = static_cast<uint32_t>(type_ptr - sql);
  // Scan back to the beginning of the RETURNS line.
  uint32_t returns_line_offset = type_offset;
  while (returns_line_offset > 0 && sql[returns_line_offset - 1] != '\n') {
    returns_line_offset--;
  }
  return ExtractDescriptionAbove(sql, returns_line_offset);
}

// Checks if a name is internal (starts with _).
bool IsInternal(const std::string& name) {
  return !name.empty() && name[0] == '_';
}

// Extracts entries from a PerfettoArgDefList node. Entry must be a struct with
// name/type/description fields (Column or Arg).
template <typename Entry>
std::vector<Entry> ExtractArgDefList(SyntaqliteParser* p,
                                     uint32_t list_id,
                                     const char* sql,
                                     const SyntaqliteComment* comments,
                                     uint32_t comment_count,
                                     uint32_t stmt_start_abs) {
  std::vector<Entry> result;
  if (!syntaqlite_node_is_present(list_id)) {
    return result;
  }
  const auto* list = static_cast<const SyntaqlitePerfettoArgDefList*>(
      syntaqlite_parser_node(p, list_id));
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    const auto* item = static_cast<const SyntaqlitePerfettoArgDef*>(
        syntaqlite_list_child(p, list, i));
    if (!item) {
      continue;
    }

    const auto* name_node = static_cast<const SyntaqliteNode*>(
        syntaqlite_parser_node(p, item->arg_name));

    Entry entry;
    entry.name = SpanText(p, name_node->ident_name.source);
    entry.type = SpanText(p, item->arg_type);

    // syntaqlite span offsets are statement-relative; use pointer arithmetic on
    // the returned pointer to get a document-absolute offset.
    uint32_t name_len = 0;
    const char* name_ptr = syntaqlite_parser_span_text(
        p, &name_node->ident_name.source, &name_len, nullptr);
    uint32_t offset = name_ptr ? static_cast<uint32_t>(name_ptr - sql) : 0;
    entry.description = ExtractInlineComment(sql, comments, comment_count,
                                             stmt_start_abs, offset);

    result.push_back(std::move(entry));
  }
  return result;
}

std::vector<Column> ExtractColumns(SyntaqliteParser* p,
                                   uint32_t list_id,
                                   const char* sql,
                                   const SyntaqliteComment* comments,
                                   uint32_t comment_count,
                                   uint32_t stmt_start_abs) {
  return ExtractArgDefList<Column>(p, list_id, sql, comments, comment_count,
                                   stmt_start_abs);
}

std::vector<Arg> ExtractArgs(SyntaqliteParser* p,
                             uint32_t list_id,
                             const char* sql,
                             const SyntaqliteComment* comments,
                             uint32_t comment_count,
                             uint32_t stmt_start_abs) {
  return ExtractArgDefList<Arg>(p, list_id, sql, comments, comment_count,
                                stmt_start_abs);
}

// Extracts macro args from a PerfettoMacroArgList node.
std::vector<Arg> ExtractMacroArgs(SyntaqliteParser* p,
                                  uint32_t list_id,
                                  const char* sql,
                                  const SyntaqliteComment* comments,
                                  uint32_t comment_count,
                                  uint32_t stmt_start_abs) {
  std::vector<Arg> result;
  if (!syntaqlite_node_is_present(list_id)) {
    return result;
  }
  const auto* list = static_cast<const SyntaqlitePerfettoMacroArgList*>(
      syntaqlite_parser_node(p, list_id));
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    const auto* item = static_cast<const SyntaqlitePerfettoMacroArg*>(
        syntaqlite_list_child(p, list, i));
    if (!item) {
      continue;
    }

    Arg arg;
    arg.name = SpanText(p, item->arg_name);
    arg.type = SpanText(p, item->arg_type);

    uint32_t name_len = 0;
    const char* name_ptr =
        syntaqlite_parser_span_text(p, &item->arg_name, &name_len, nullptr);
    uint32_t arg_offset = name_ptr ? static_cast<uint32_t>(name_ptr - sql) : 0;
    arg.description = ExtractInlineComment(sql, comments, comment_count,
                                           stmt_start_abs, arg_offset);

    result.push_back(std::move(arg));
  }
  return result;
}

}  // namespace

ParsedModule ParseStdlibModule(const char* sql, uint32_t sql_len) {
  ParsedModule result;

  SyntaqliteParser* p = syntaqlite_parser_create_with_dialect(
      nullptr, syntaqlite_perfetto_dialect());
  PERFETTO_CHECK(p != nullptr);

  // Enable comment collection so we can extract documentation.
  syntaqlite_parser_set_collect_tokens(p, 1);
  // Enable node extent tracking so we can get byte offsets of AST nodes.
  syntaqlite_parser_set_collect_node_extents(p, 1);
  // Enable macro fallback so we don't fail on unregistered macros.
  syntaqlite_parser_set_macro_fallback(p, 1);

  syntaqlite_parser_reset(p, sql, sql_len);

  for (;;) {
    int32_t rc = syntaqlite_parser_next(p);
    if (rc == SYNTAQLITE_PARSE_DONE) {
      break;
    }

    // Get comments for this statement.
    uint32_t comment_count = 0;
    const SyntaqliteComment* comments =
        syntaqlite_result_comments(p, &comment_count);

    if (rc == SYNTAQLITE_PARSE_ERROR) {
      const char* err_msg = syntaqlite_result_error_msg(p);
      if (err_msg) {
        result.errors.push_back(std::string("Parse error: ") + err_msg);
      } else {
        result.errors.push_back("Unknown parse error");
      }
      continue;
    }

    uint32_t root = syntaqlite_result_root(p);
    if (!syntaqlite_node_is_present(root)) {
      continue;
    }

    const auto* node =
        static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, root));

    // Get the document-absolute byte offset of the CREATE keyword for
    // description extraction. syntaqlite node/span offsets are
    // statement-relative; use pointer arithmetic on the returned text pointer.
    uint32_t stmt_len = 0;
    const char* stmt_ptr =
        syntaqlite_parser_node_text(p, root, &stmt_len, nullptr);
    PERFETTO_DCHECK(stmt_ptr != nullptr && stmt_len > 0);
    uint32_t stmt_offset =
        (stmt_ptr && stmt_len > 0) ? static_cast<uint32_t>(stmt_ptr - sql) : 0;

    // Get the document-absolute byte offset of the statement source start
    // (stmt_source in syntaqlite terms). All comment offsets are measured from
    // this point, so this is what we add to SyntaqliteComment.offset to get
    // absolute comment positions.
    uint32_t stmt_src_abs = 0;
    syntaqlite_parser_text(p, &stmt_src_abs, nullptr);

    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT: {
        const auto& n = node->create_perfetto_table_stmt;
        TableOrView tv;
        tv.name = SpanText(p, n.table_name);
        tv.type = "TABLE";
        tv.exposed = !IsInternal(tv.name);
        tv.description = ExtractDescriptionAbove(sql, stmt_offset);
        tv.columns = ExtractColumns(p, n.schema, sql, comments, comment_count,
                                    stmt_src_abs);
        result.table_views.push_back(std::move(tv));
        break;
      }

      case SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT: {
        const auto& n = node->create_perfetto_view_stmt;
        TableOrView tv;
        tv.name = SpanText(p, n.view_name);
        tv.type = "VIEW";
        tv.exposed = !IsInternal(tv.name);
        tv.description = ExtractDescriptionAbove(sql, stmt_offset);
        tv.columns = ExtractColumns(p, n.schema, sql, comments, comment_count,
                                    stmt_src_abs);
        result.table_views.push_back(std::move(tv));
        break;
      }

      case SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT:
      case SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT: {
        // Both node types share the same fields we care about: function_name,
        // args, and return_type. Access them via the appropriate union member.
        SyntaqliteTextSpan fn_name_span;
        uint32_t args_list_id;
        uint32_t return_type_id;
        if (node->tag == SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT) {
          const auto& n = node->create_perfetto_function_stmt;
          fn_name_span = n.function_name;
          args_list_id = n.args;
          return_type_id = n.return_type;
        } else {
          const auto& n = node->create_perfetto_delegating_function_stmt;
          fn_name_span = n.function_name;
          args_list_id = n.args;
          return_type_id = n.return_type;
        }

        Function fn;
        fn.name = SpanText(p, fn_name_span);
        fn.exposed = !IsInternal(fn.name);
        fn.description = ExtractDescriptionAbove(sql, stmt_offset);
        fn.args = ExtractArgs(p, args_list_id, sql, comments, comment_count,
                              stmt_src_abs);

        if (syntaqlite_node_is_present(return_type_id)) {
          const auto* rt = static_cast<const SyntaqlitePerfettoReturnType*>(
              syntaqlite_parser_node(p, return_type_id));
          if (rt->kind == SYNTAQLITE_PERFETTO_RETURN_KIND_TABLE) {
            fn.is_table_function = true;
            fn.return_type = "TABLE";
            fn.columns = ExtractColumns(p, rt->table_columns, sql, comments,
                                        comment_count, stmt_src_abs);
          } else {
            fn.return_type = SpanText(p, rt->scalar_type);
          }
          fn.return_description = ExtractReturnDescription(sql, p, rt);
        }

        result.functions.push_back(std::move(fn));
        break;
      }

      case SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT: {
        const auto& n = node->create_perfetto_macro_stmt;
        Macro macro;
        macro.name = SpanText(p, n.macro_name);
        macro.exposed = !IsInternal(macro.name);
        macro.description = ExtractDescriptionAbove(sql, stmt_offset);
        macro.return_type = SpanText(p, n.return_type);
        macro.args = ExtractMacroArgs(p, n.args, sql, comments, comment_count,
                                      stmt_src_abs);
        result.macros.push_back(std::move(macro));
        break;
      }

      default:
        // INCLUDE PERFETTO MODULE, DROP PERFETTO INDEX, plain SQL, etc.
        // We don't document these.
        break;
    }
  }

  syntaqlite_parser_destroy(p);
  return result;
}

}  // namespace perfetto::trace_processor::stdlib_doc
