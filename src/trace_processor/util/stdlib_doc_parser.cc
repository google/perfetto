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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::trace_processor::stdlib_doc {

namespace {

// Returns the authored source text for a span as a string_view into the
// original input — no allocation. Uses span_text (not expanded_text) so
// macro-expanded names resolve back to the call-site source, not the
// internal expansion buffer.
std::string_view SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  uint32_t len = 0;
  const char* text = syntaqlite_parser_span_text(p, &span, &len, nullptr);
  if (!text) {
    return {};
  }
  return {text, len};
}

// Strips leading "-- " or "--" from a single comment line.
std::string StripCommentPrefix(const char* text, uint32_t len) {
  std::string_view s = base::TrimWhitespace(std::string_view(text, len));
  if (s.size() >= 2 && s[0] == '-' && s[1] == '-') {
    s.remove_prefix(2);
  }
  return std::string(base::TrimWhitespace(s));
}

// Joins all line comments (kind == 0) from |comments| into a single string,
// space-separated. |stmt_ptr| is the base pointer for comment offsets.
std::string JoinLineComments(const char* stmt_ptr,
                             const SyntaqliteComment* comments,
                             uint32_t count) {
  std::vector<std::string> parts;
  for (uint32_t i = 0; i < count; i++) {
    if (comments[i].kind != 0) {
      continue;
    }
    std::string s =
        StripCommentPrefix(stmt_ptr + comments[i].offset, comments[i].length);
    if (!s.empty()) {
      parts.push_back(std::move(s));
    }
  }
  return base::Join(parts, " ");
}

// Returns the last contiguous block of line comments (no blank line between
// consecutive comments). Used for statement descriptions so that a license
// header separated from the doc comment by a blank line is excluded.
std::string LastContiguousLineComments(const char* stmt_ptr,
                                       const SyntaqliteComment* comments,
                                       uint32_t count) {
  if (count == 0) {
    return {};
  }
  // Walk forward, recording where each new "block" starts (a block is broken
  // by a blank line or a block comment).
  uint32_t block_start = 0;
  for (uint32_t i = 1; i < count; i++) {
    if (comments[i - 1].kind != 0 || comments[i].kind != 0) {
      block_start = i;
      continue;
    }
    // Count newlines in the gap between end of comments[i-1] and start of
    // comments[i]. Two or more newlines mean there is a blank line between
    // them.
    uint32_t gap_start = comments[i - 1].offset + comments[i - 1].length;
    uint32_t gap_end = comments[i].offset;
    int newlines = 0;
    for (uint32_t j = gap_start; j < gap_end; j++) {
      if (stmt_ptr[j] == '\n' && ++newlines >= 2) {
        block_start = i;
        break;
      }
    }
  }
  return JoinLineComments(stmt_ptr, comments + block_start,
                          count - block_start);
}

// Maps a source pointer (returned by a syntaqlite span/node accessor) to the
// index of the corresponding token in the per-statement token array. Returns
// UINT32_MAX if not found.
// O(token_count) per call. Stdlib modules are small (hundreds of tokens), so
// this is acceptable even when called once per arg.
SyntaqliteTokenIdx SpanToTokenIdx(const char* span_ptr,
                                  const char* stmt_ptr,
                                  const SyntaqliteParserToken* tokens,
                                  uint32_t count) {
  if (!span_ptr || !stmt_ptr || span_ptr < stmt_ptr) {
    return UINT32_MAX;
  }
  uint32_t stmt_rel = static_cast<uint32_t>(span_ptr - stmt_ptr);
  for (uint32_t i = 0; i < count; i++) {
    if (tokens[i]._layer_id == 0 && tokens[i].offset == stmt_rel) {
      return i;
    }
  }
  return UINT32_MAX;
}

// Checks if a name is internal (starts with _).
bool IsInternal(const std::string& name) {
  return base::StartsWith(name, "_");
}

// Extracts entries from a PerfettoArgDefList node. Entry must be a struct with
// name/type/description fields (Column or Arg).
template <typename Entry>
std::vector<Entry> ExtractArgDefList(SyntaqliteParser* p,
                                     uint32_t list_id,
                                     const char* stmt_ptr,
                                     const SyntaqliteParserToken* tokens,
                                     uint32_t token_count) {
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

    uint32_t name_len = 0;
    const char* name_ptr = syntaqlite_parser_span_text(
        p, &name_node->ident_name.source, &name_len, nullptr);
    SyntaqliteTokenIdx tok_idx =
        SpanToTokenIdx(name_ptr, stmt_ptr, tokens, token_count);
    if (tok_idx != UINT32_MAX) {
      uint32_t c_count = 0;
      const auto* cs = syntaqlite_token_leading_comments(p, tok_idx, &c_count);
      entry.description = JoinLineComments(stmt_ptr, cs, c_count);
    }

    result.push_back(std::move(entry));
  }
  return result;
}

std::vector<Column> ExtractColumns(SyntaqliteParser* p,
                                   uint32_t list_id,
                                   const char* stmt_ptr,
                                   const SyntaqliteParserToken* tokens,
                                   uint32_t token_count) {
  return ExtractArgDefList<Column>(p, list_id, stmt_ptr, tokens, token_count);
}

std::vector<Arg> ExtractArgs(SyntaqliteParser* p,
                             uint32_t list_id,
                             const char* stmt_ptr,
                             const SyntaqliteParserToken* tokens,
                             uint32_t token_count) {
  return ExtractArgDefList<Arg>(p, list_id, stmt_ptr, tokens, token_count);
}

// Extracts macro args from a PerfettoMacroArgList node.
std::vector<Arg> ExtractMacroArgs(SyntaqliteParser* p,
                                  uint32_t list_id,
                                  const char* stmt_ptr,
                                  const SyntaqliteParserToken* tokens,
                                  uint32_t token_count) {
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
    SyntaqliteTokenIdx tok_idx =
        SpanToTokenIdx(name_ptr, stmt_ptr, tokens, token_count);
    if (tok_idx != UINT32_MAX) {
      uint32_t c_count = 0;
      const auto* cs = syntaqlite_token_leading_comments(p, tok_idx, &c_count);
      arg.description = JoinLineComments(stmt_ptr, cs, c_count);
    }

    result.push_back(std::move(arg));
  }
  return result;
}

// Returns the return description for a function: the leading line comments on
// the RETURNS keyword. The return type node's first token is found via the
// token array; RETURNS immediately precedes it.
std::string GetReturnDescription(SyntaqliteParser* p,
                                 uint32_t return_type_node_id,
                                 const char* stmt_ptr,
                                 const SyntaqliteParserToken* tokens,
                                 uint32_t token_count) {
  if (!syntaqlite_node_is_present(return_type_node_id)) {
    return {};
  }
  uint32_t rt_len = 0;
  const char* rt_ptr =
      syntaqlite_parser_node_text(p, return_type_node_id, &rt_len, nullptr);
  if (!rt_ptr) {
    return {};
  }
  SyntaqliteTokenIdx rt_tok_idx =
      SpanToTokenIdx(rt_ptr, stmt_ptr, tokens, token_count);
  if (rt_tok_idx == UINT32_MAX) {
    return {};
  }
  // Try leading comments on the return type's first token (covers the case
  // where the node extent begins at RETURNS). If empty, try the preceding
  // token (covers the case where the node extent begins after RETURNS).
  uint32_t c_count = 0;
  const auto* cs = syntaqlite_token_leading_comments(p, rt_tok_idx, &c_count);
  if (c_count == 0 && rt_tok_idx > 0) {
    cs = syntaqlite_token_leading_comments(p, rt_tok_idx - 1, &c_count);
  }
  return JoinLineComments(stmt_ptr, cs, c_count);
}

struct SyntaqliteParserDeleter {
  void operator()(SyntaqliteParser* p) const { syntaqlite_parser_destroy(p); }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, SyntaqliteParserDeleter>;

}  // namespace

ParsedModule ParseStdlibModule(const char* sql, uint32_t sql_len) {
  ParsedModule result;

  ScopedParser owned(syntaqlite_parser_create_with_dialect(
      nullptr, syntaqlite_perfetto_dialect()));
  PERFETTO_CHECK(owned != nullptr);
  SyntaqliteParser* p = owned.get();

  syntaqlite_parser_set_collect_tokens(p, 1);
  syntaqlite_parser_set_collect_node_extents(p, 1);
  syntaqlite_parser_set_macro_fallback(p, 1);

  syntaqlite_parser_reset(p, sql, sql_len);

  for (;;) {
    int32_t rc = syntaqlite_parser_next(p);
    if (rc == SYNTAQLITE_PARSE_DONE) {
      break;
    }

    if (rc == SYNTAQLITE_PARSE_ERROR) {
      const char* err_msg = syntaqlite_result_error_msg(p);
      result.errors.push_back(err_msg ? std::string("Parse error: ") + err_msg
                                      : std::string("Unknown parse error"));
      continue;
    }

    uint32_t root = syntaqlite_result_root(p);
    if (!syntaqlite_node_is_present(root)) {
      continue;
    }

    const auto* node =
        static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, root));

    // syntaqlite_parser_text with null out-params returns the raw statement
    // start pointer without writing any lengths; offsets in tokens/comments
    // are all relative to this base.
    const char* stmt_ptr = syntaqlite_parser_text(p, nullptr, nullptr);
    PERFETTO_DCHECK(stmt_ptr != nullptr);

    uint32_t token_count = 0;
    const SyntaqliteParserToken* tokens =
        syntaqlite_result_tokens(p, &token_count);

    // Statement-level description: last contiguous block of leading line
    // comments on token 0 (the CREATE keyword), skipping license headers
    // separated by a blank line.
    auto get_stmt_desc = [&]() -> std::string {
      uint32_t count = 0;
      const auto* cs = syntaqlite_token_leading_comments(p, 0, &count);
      return LastContiguousLineComments(stmt_ptr, cs, count);
    };

    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT: {
        const auto& n = node->create_perfetto_table_stmt;
        TableOrView tv;
        tv.name = SpanText(p, n.table_name);
        tv.type = "TABLE";
        tv.exposed = !IsInternal(tv.name);
        tv.description = get_stmt_desc();
        tv.columns = ExtractColumns(p, n.schema, stmt_ptr, tokens, token_count);
        result.table_views.push_back(std::move(tv));
        break;
      }

      case SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT: {
        const auto& n = node->create_perfetto_view_stmt;
        TableOrView tv;
        tv.name = SpanText(p, n.view_name);
        tv.type = "VIEW";
        tv.exposed = !IsInternal(tv.name);
        tv.description = get_stmt_desc();
        tv.columns = ExtractColumns(p, n.schema, stmt_ptr, tokens, token_count);
        result.table_views.push_back(std::move(tv));
        break;
      }

      case SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT:
      case SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT: {
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
        fn.description = get_stmt_desc();
        fn.args = ExtractArgs(p, args_list_id, stmt_ptr, tokens, token_count);

        if (syntaqlite_node_is_present(return_type_id)) {
          const auto* rt = static_cast<const SyntaqlitePerfettoReturnType*>(
              syntaqlite_parser_node(p, return_type_id));
          if (rt->kind == SYNTAQLITE_PERFETTO_RETURN_KIND_TABLE) {
            fn.is_table_function = true;
            fn.return_type = "TABLE";
            fn.columns = ExtractColumns(p, rt->table_columns, stmt_ptr, tokens,
                                        token_count);
          } else {
            fn.return_type = SpanText(p, rt->scalar_type);
          }
          fn.return_description = GetReturnDescription(
              p, return_type_id, stmt_ptr, tokens, token_count);
        }

        result.functions.push_back(std::move(fn));
        break;
      }

      case SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT: {
        const auto& n = node->create_perfetto_macro_stmt;
        Macro macro;
        macro.name = SpanText(p, n.macro_name);
        macro.exposed = !IsInternal(macro.name);
        macro.description = get_stmt_desc();
        macro.return_type = SpanText(p, n.return_type);
        macro.args = ExtractMacroArgs(p, n.args, stmt_ptr, tokens, token_count);

        // Return description: leading comments on the RETURNS keyword.
        // GetReturnDescription() is not reused here because macros expose
        // the return type as a SyntaqliteTextSpan (not a node id), so we
        // must use span_text instead of node_text to locate the token.
        uint32_t ret_len = 0;
        const char* ret_ptr =
            syntaqlite_parser_span_text(p, &n.return_type, &ret_len, nullptr);
        SyntaqliteTokenIdx ret_tok_idx =
            SpanToTokenIdx(ret_ptr, stmt_ptr, tokens, token_count);
        if (ret_tok_idx != UINT32_MAX && ret_tok_idx > 0) {
          uint32_t c_count = 0;
          const auto* cs =
              syntaqlite_token_leading_comments(p, ret_tok_idx - 1, &c_count);
          macro.return_description = JoinLineComments(stmt_ptr, cs, c_count);
        }

        result.macros.push_back(std::move(macro));
        break;
      }

      default:
        break;
    }
  }

  return result;
}

}  // namespace perfetto::trace_processor::stdlib_doc
