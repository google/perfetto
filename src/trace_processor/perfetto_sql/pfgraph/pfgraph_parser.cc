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

#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_parser.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <variant>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor::pfgraph {

namespace {

// Case-insensitive string compare.
bool IdentIs(std::string_view text, const char* keyword) {
  return base::CaseInsensitiveEqual(std::string(text), std::string(keyword));
}

// Extract the string content from a quoted token (strip quotes).
std::string UnquoteString(std::string_view tok_text) {
  if (tok_text.size() >= 6 && tok_text.substr(0, 3) == "'''") {
    return std::string(tok_text.substr(3, tok_text.size() - 6));
  }
  if (tok_text.size() >= 2 && tok_text.front() == '\'') {
    std::string result;
    // Handle escaped quotes ('').
    for (size_t i = 1; i < tok_text.size() - 1; ++i) {
      if (tok_text[i] == '\'' && i + 1 < tok_text.size() - 1 &&
          tok_text[i + 1] == '\'') {
        result += '\'';
        ++i;
      } else {
        result += tok_text[i];
      }
    }
    return result;
  }
  return std::string(tok_text);
}

class Parser {
 public:
  explicit Parser(std::string_view input) : tok_(input) {}

  base::StatusOr<GraphModule> Parse() {
    GraphModule mod;

    // Parse optional module declaration.
    if (PeekIdent("module")) {
      tok_.Next();  // consume "module"
      ASSIGN_OR_RETURN(mod.module_name, ParseDottedName());
    }

    // Parse imports.
    while (PeekIdent("import")) {
      tok_.Next();  // consume "import"
      std::string name;
      ASSIGN_OR_RETURN(name, ParseDottedName());
      mod.imports.push_back(std::move(name));
    }

    // Parse declarations (named pipelines or @sql blocks).
    while (tok_.Peek().type != TokenType::kEof) {
      Token t = tok_.Peek();

      // @annotation
      if (t.type == TokenType::kAt) {
        tok_.Next();  // consume @
        Token ann = tok_.Next();
        if (ann.type != TokenType::kIdent) {
          return ErrorAt(ann, "expected annotation name after @");
        }

        if (IdentIs(ann.text, "sql")) {
          SqlBlock block;
          ASSIGN_OR_RETURN(block, ParseSqlBlock(ann.line));
          mod.declarations.push_back(std::move(block));
        } else if (IdentIs(ann.text, "table") || IdentIs(ann.text, "view")) {
          PipelineAnnotation pa = IdentIs(ann.text, "table")
                                      ? PipelineAnnotation::kTable
                                      : PipelineAnnotation::kView;
          NamedPipeline pipeline;
          ASSIGN_OR_RETURN(pipeline, ParseNamedPipeline(pa));
          mod.declarations.push_back(std::move(pipeline));
        } else if (IdentIs(ann.text, "function")) {
          FunctionDecl func;
          ASSIGN_OR_RETURN(func, ParseFunctionDecl(ann.line));
          mod.declarations.push_back(std::move(func));
        } else if (IdentIs(ann.text, "define")) {
          TemplateDecl tmpl;
          ASSIGN_OR_RETURN(tmpl, ParseTemplateDecl(ann.line));
          mod.declarations.push_back(std::move(tmpl));
        } else {
          return ErrorAt(ann, "unknown annotation @" +
                                  std::string(ann.text) +
                                  " (expected @table, @view, @function, "
                                  "@define, or @sql)");
        }
      } else if (t.type == TokenType::kIdent) {
        // Could be a named pipeline (name: ...).
        // We need to check if the next token after the ident is a colon.
        Token ident = tok_.Next();
        Token maybe_colon = tok_.Peek();
        if (maybe_colon.type == TokenType::kColon) {
          // Put the ident back conceptually - we'll use it.
          tok_.Next();  // consume the colon
          NamedPipeline pipeline;
          ASSIGN_OR_RETURN(pipeline,
                           ParsePipelineBody(std::string(ident.text),
                                             PipelineAnnotation::kNone,
                                             ident.line));
          mod.declarations.push_back(std::move(pipeline));
        } else {
          return ErrorAt(ident,
                         "unexpected identifier '" + std::string(ident.text) +
                             "' at top level (expected 'name:' or @annotation)");
        }
      } else {
        return ErrorAt(t, "unexpected token at top level");
      }
    }

    return mod;
  }

 private:
  // Helpers.
  bool PeekIdent(const char* keyword) {
    Token t = tok_.Peek();
    return t.type == TokenType::kIdent && IdentIs(t.text, keyword);
  }

  bool PeekType(TokenType type) { return tok_.Peek().type == type; }

  base::StatusOr<Token> Expect(TokenType type, const char* context) {
    Token t = tok_.Next();
    if (t.type != type) {
      return ErrorAt(t, std::string("expected ") + context + " but got '" +
                            std::string(t.text) + "'");
    }
    return t;
  }

  // ExpectIdent removed — unused for now but may be useful later.

  base::Status ErrorAt(const Token& t, const std::string& msg) {
    return base::ErrStatus("pfgraph:%u:%u: %s", t.line, t.col, msg.c_str());
  }

  // Parse a dotted name (e.g., "android.binder").
  base::StatusOr<std::string> ParseDottedName() {
    Token first = tok_.Next();
    if (first.type != TokenType::kIdent) {
      return ErrorAt(first, "expected identifier in dotted name");
    }
    std::string result(first.text);
    while (tok_.Peek().type == TokenType::kDot) {
      tok_.Next();  // consume dot
      Token part = tok_.Next();
      if (part.type != TokenType::kIdent) {
        return ErrorAt(part, "expected identifier after '.' in dotted name");
      }
      result += ".";
      result += part.text;
    }
    return result;
  }

  // Parse @sql { ... } block.
  base::StatusOr<SqlBlock> ParseSqlBlock(uint32_t line) {
    RETURN_IF_ERROR(Expect(TokenType::kLBrace, "'{'").status());

    // Use raw scanning to preserve verbatim SQL text.
    SqlBlock block;
    block.sql = tok_.ScanUntilMatchingBrace();
    block.line = line;
    return block;
  }

  // Parse a named pipeline with annotation already consumed.
  base::StatusOr<NamedPipeline> ParseNamedPipeline(PipelineAnnotation ann) {
    Token name;
    ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "pipeline name"));
    // Skip optional column schema: name(col TYPE, col TYPE, ...) before ':'
    if (PeekType(TokenType::kLParen)) {
      int depth = 0;
      while (true) {
        Token t = tok_.Next();
        if (t.type == TokenType::kLParen) ++depth;
        if (t.type == TokenType::kRParen) --depth;
        if (depth == 0) break;
        if (t.type == TokenType::kEof)
          return ErrorAt(t, "unterminated column schema");
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
    return ParsePipelineBody(std::string(name.text), ann, name.line);
  }

  // Parse pipeline body (source + operations). Name and colon already consumed.
  base::StatusOr<NamedPipeline> ParsePipelineBody(std::string name,
                                                   PipelineAnnotation ann,
                                                   uint32_t line) {
    NamedPipeline np;
    np.name = std::move(name);
    np.annotation = ann;
    np.line = line;

    // Handle @sql { ... } as the pipeline body (raw SQL passthrough).
    if (tok_.Peek().type == TokenType::kAt) {
      tok_.Next();  // consume @
      Token ann_tok = tok_.Next();
      if (IdentIs(ann_tok.text, "sql")) {
        RETURN_IF_ERROR(Expect(TokenType::kLBrace, "'{'").status());
        std::string raw_sql = tok_.ScanUntilMatchingBrace();
        SqlSource src;
        src.sql = raw_sql;
        np.pipeline.source = Source{std::move(src)};
        return np;
      }
      return ErrorAt(ann_tok, "expected 'sql' after '@' in pipeline body");
    }

    ASSIGN_OR_RETURN(np.pipeline.source, ParseSource());

    // Parse chained operations (.op(...)).
    while (tok_.Peek().type == TokenType::kDot) {
      tok_.Next();  // consume dot
      Operation op;
      ASSIGN_OR_RETURN(op, ParseOperation());
      np.pipeline.operations.push_back(std::move(op));
    }

    return np;
  }

  // Parse a source expression.
  base::StatusOr<Source> ParseSource() {
    Token t = tok_.Peek();
    if (t.type != TokenType::kIdent) {
      return ErrorAt(t, "expected source (table, slices, sql, join, union, "
                        "interval_intersect, or pipeline reference)");
    }

    if (IdentIs(t.text, "table")) {
      return ParseTableSource();
    }
    if (IdentIs(t.text, "slices")) {
      return ParseSlicesSource();
    }
    if (IdentIs(t.text, "sql")) {
      return ParseSqlSource();
    }
    if (IdentIs(t.text, "time_range")) {
      return ParseTimeRangeSource();
    }
    if (IdentIs(t.text, "interval_intersect")) {
      return ParseIntervalIntersectSource();
    }
    if (IdentIs(t.text, "join")) {
      return ParseJoinSource();
    }
    if (IdentIs(t.text, "union")) {
      return ParseUnionSource();
    }
    if (IdentIs(t.text, "create_slices")) {
      return ParseCreateSlicesSource();
    }
    if (IdentIs(t.text, "lookup_table")) {
      return ParseLookupTableSource();
    }

    // Check if this is a template call (ident followed by '(') or a pipeline
    // reference (ident followed by anything else like '.', ':', AS, etc.).
    tok_.Next();  // consume the ident
    if (tok_.Peek().type == TokenType::kLParen) {
      return ParseTemplateCallSource(std::string(t.text));
    }
    // Not a template call; put the ident back by parsing as pipeline ref.
    // We already consumed the ident, so manually construct the ref.
    PipelineRef ref;
    ref.name = std::string(t.text);
    if (PeekIdent("AS")) {
      tok_.Next();
      Token alias = tok_.Next();
      ref.alias = std::string(alias.text);
    }
    return Source{std::move(ref)};
  }

  base::StatusOr<Source> ParseTableSource() {
    tok_.Next();  // consume "table"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    Token name;
    ASSIGN_OR_RETURN(name, Expect(TokenType::kString, "table name"));
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    TableSource src;
    src.table_name = UnquoteString(name.text);
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseSlicesSource() {
    tok_.Next();  // consume "slices"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    SlicesSource src;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      Token val;
      ASSIGN_OR_RETURN(val, Expect(TokenType::kString, "string value"));
      std::string value = UnquoteString(val.text);
      if (IdentIs(key.text, "name")) {
        src.name_glob = value;
      } else if (IdentIs(key.text, "thread")) {
        src.thread_glob = value;
      } else if (IdentIs(key.text, "process")) {
        src.process_glob = value;
      } else if (IdentIs(key.text, "track")) {
        src.track_glob = value;
      } else {
        return ErrorAt(key, "unknown slices parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseSqlSource() {
    tok_.Next();  // consume "sql"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    Token sql_str;
    ASSIGN_OR_RETURN(sql_str, Expect(TokenType::kString, "SQL string"));
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    SqlSource src;
    src.sql = UnquoteString(sql_str.text);
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseTimeRangeSource() {
    tok_.Next();  // consume "time_range"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    TimeRangeSource src;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "ts")) {
        Token val;
        ASSIGN_OR_RETURN(val, Expect(TokenType::kInt, "integer"));
        src.ts = static_cast<int64_t>(std::stoll(std::string(val.text)));
      } else if (IdentIs(key.text, "dur")) {
        Token val;
        ASSIGN_OR_RETURN(val, Expect(TokenType::kInt, "integer"));
        src.dur = static_cast<int64_t>(std::stoll(std::string(val.text)));
      } else if (IdentIs(key.text, "dynamic")) {
        Token val;
        ASSIGN_OR_RETURN(val, Expect(TokenType::kIdent, "true/false"));
        src.dynamic = IdentIs(val.text, "true");
      } else {
        return ErrorAt(key, "unknown time_range parameter");
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseIntervalIntersectSource() {
    tok_.Next();  // consume "interval_intersect"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    IntervalIntersectSource src;
    // Parse input refs until we hit 'partition:' or ')'.
    while (!PeekType(TokenType::kRParen)) {
      if (PeekIdent("partition")) {
        tok_.Next();
        RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
        ASSIGN_OR_RETURN(src.partition_columns, ParseBracketedList());
        break;
      }
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref, ParsePipelineRef());
      src.inputs.push_back(std::move(ref));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    if (src.inputs.size() < 2) {
      return base::ErrStatus(
          "pfgraph: interval_intersect requires at least 2 inputs");
    }
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseJoinSource() {
    tok_.Next();  // consume "join"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    JoinSource src;

    // Parse left ref.
    ASSIGN_OR_RETURN(src.left, ParsePipelineRef());
    if (PeekType(TokenType::kComma))
      tok_.Next();

    // Parse right ref.
    ASSIGN_OR_RETURN(src.right, ParsePipelineRef());
    if (PeekType(TokenType::kComma))
      tok_.Next();

    // Parse keyword arguments.
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());

      if (IdentIs(key.text, "on")) {
        // Parse join condition: either "left_col = right_col" or freeform.
        std::string cond;
        ASSIGN_OR_RETURN(cond, ParseBalancedExpr());
        // Try to split on " = " for simple equality.
        auto eq_pos = cond.find(" = ");
          bool has_complex = cond.find("BETWEEN") != std::string::npos || cond.find(" AND ") != std::string::npos || cond.find(" OR ") != std::string::npos;
        if (!has_complex && eq_pos != std::string::npos &&
            cond.find(" = ", eq_pos + 1) == std::string::npos) {
          src.on_left_col = cond.substr(0, eq_pos);
          src.on_right_col = cond.substr(eq_pos + 3);
          // Trim whitespace.
          while (!src.on_left_col.empty() && src.on_left_col.back() == ' ')
            src.on_left_col.pop_back();
          while (!src.on_right_col.empty() && src.on_right_col.front() == ' ')
            src.on_right_col.erase(src.on_right_col.begin());
        } else {
          src.on_expr = cond;
        }
      } else if (IdentIs(key.text, "type")) {
        Token val = tok_.Next();
        src.is_left_join = IdentIs(val.text, "LEFT");
      } else {
        return ErrorAt(key, "unknown join parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseUnionSource() {
    tok_.Next();  // consume "union"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    UnionSource src;
    while (!PeekType(TokenType::kRParen)) {
      // Check for union_all keyword param.
      if (PeekIdent("all")) {
        tok_.Next();
        RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
        Token val = tok_.Next();
        src.union_all = IdentIs(val.text, "true");
      } else {
        PipelineRef ref;
        ASSIGN_OR_RETURN(ref, ParsePipelineRef());
        src.inputs.push_back(std::move(ref));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  base::StatusOr<Source> ParseCreateSlicesSource() {
    tok_.Next();  // consume "create_slices"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    CreateSlicesSource src;
    src.starts_ts_col = "ts";
    src.ends_ts_col = "ts";
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "starts")) {
        ASSIGN_OR_RETURN(src.starts, ParsePipelineRef());
      } else if (IdentIs(key.text, "ends")) {
        ASSIGN_OR_RETURN(src.ends, ParsePipelineRef());
      } else if (IdentIs(key.text, "starts_ts")) {
        Token val = tok_.Next();
        src.starts_ts_col = std::string(val.text);
      } else if (IdentIs(key.text, "ends_ts")) {
        Token val = tok_.Next();
        src.ends_ts_col = std::string(val.text);
      } else {
        return ErrorAt(key, "unknown create_slices parameter");
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  // Parse a pipeline reference: "name", "name AS alias", or "table('name') AS alias".
  base::StatusOr<PipelineRef> ParsePipelineRef() {
    Token name_tok = tok_.Next();
    if (name_tok.type != TokenType::kIdent) {
      return ErrorAt(name_tok, "expected pipeline name");
    }
    // Handle inline table('name') expressions.
    if (IdentIs(name_tok.text, "table") && PeekType(TokenType::kLParen)) {
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      Token tname;
      ASSIGN_OR_RETURN(tname, Expect(TokenType::kString, "table name"));
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
      PipelineRef ref;
      ref.name = UnquoteString(tname.text);
      if (PeekIdent("AS")) {
        tok_.Next();
        Token alias = tok_.Next();
        ref.alias = std::string(alias.text);
      }
      return ref;
    }
    PipelineRef ref;
    ref.name = std::string(name_tok.text);
    if (PeekIdent("AS")) {
      tok_.Next();
      Token alias = tok_.Next();
      ref.alias = std::string(alias.text);
    }
    return ref;
  }

  // Parse a [bracketed, list, of, identifiers].
  base::StatusOr<std::vector<std::string>> ParseBracketedList() {
    RETURN_IF_ERROR(Expect(TokenType::kLBracket, "'['").status());
    std::vector<std::string> items;
    while (!PeekType(TokenType::kRBracket)) {
      Token item = tok_.Next();
      if (item.type != TokenType::kIdent) {
        return ErrorAt(item, "expected identifier in list");
      }
      items.push_back(std::string(item.text));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRBracket, "']'").status());
    return items;
  }

  // Parse a single operation after a dot.
  base::StatusOr<Operation> ParseOperation() {
    Token op_name = tok_.Next();
    if (op_name.type != TokenType::kIdent) {
      return ErrorAt(op_name, "expected operation name after '.'");
    }

    if (IdentIs(op_name.text, "filter")) {
      return ParseFilterOp();
    }
    if (IdentIs(op_name.text, "select")) {
      return ParseSelectOp();
    }
    if (IdentIs(op_name.text, "add_columns")) {
      return ParseAddColumnsOp();
    }
    if (IdentIs(op_name.text, "group_by")) {
      return ParseGroupByOp();
    }
    if (IdentIs(op_name.text, "sort")) {
      return ParseSortOp();
    }
    if (IdentIs(op_name.text, "limit")) {
      return ParseLimitOp();
    }
    if (IdentIs(op_name.text, "offset")) {
      return ParseOffsetOp();
    }
    if (IdentIs(op_name.text, "counter_to_intervals")) {
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
      return Operation{CounterToIntervalsOp{}};
    }
    if (IdentIs(op_name.text, "filter_during")) {
      return ParseFilterDuringOp();
    }
    if (IdentIs(op_name.text, "filter_in")) {
      return ParseFilterInOp();
    }
    if (IdentIs(op_name.text, "window")) {
      return ParseWindowOp();
    }
    if (IdentIs(op_name.text, "computed")) {
      return ParseComputedOp();
    }
    if (IdentIs(op_name.text, "classify")) {
      return ParseClassifyOp();
    }
    if (IdentIs(op_name.text, "extract_args")) {
      return ParseExtractArgsOp();
    }
    if (IdentIs(op_name.text, "distinct")) {
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
      return Operation{DistinctOp{}};
    }
    if (IdentIs(op_name.text, "except")) {
      return ParseExceptOp();
    }
    if (IdentIs(op_name.text, "span_join")) {
      return ParseSpanJoinOp();
    }
    if (IdentIs(op_name.text, "unpivot")) {
      return ParseUnpivotOp();
    }
    if (IdentIs(op_name.text, "index")) {
      return ParseIndexOp();
    }
    if (IdentIs(op_name.text, "parse_name")) {
      return ParseParseNameOp();
    }
    if (IdentIs(op_name.text, "closest_preceding")) {
      return ParseClosestPrecedingOp();
    }
    if (IdentIs(op_name.text, "flow_reachable")) {
      return ParseFlowReachableOp();
    }
    if (IdentIs(op_name.text, "flatten_intervals")) {
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
      return Operation{FlattenIntervalsOp{}};
    }
    if (IdentIs(op_name.text, "merge_overlapping")) {
      return ParseMergeOverlappingOp();
    }
    if (IdentIs(op_name.text, "graph_reachable")) {
      return ParseGraphReachableOp();
    }
    if (IdentIs(op_name.text, "find_ancestor")) {
      return ParseFindAncestorOp();
    }
    if (IdentIs(op_name.text, "find_descendant")) {
      return ParseFindDescendantOp();
    }
    if (IdentIs(op_name.text, "join")) {
      return ParseJoinOp();
    }
    if (IdentIs(op_name.text, "cross_join")) {
      return ParseCrossJoinOp();
    }
    if (IdentIs(op_name.text, "agg")) {
      return ErrorAt(op_name,
                     ".agg() must follow .group_by(), not used standalone");
    }

    // Unknown operation name: treat as a template call.
    return ParseTemplateCallOp(std::string(op_name.text));
  }

  // .filter(EXPR) - collect everything inside parens as SQL expression.
  base::StatusOr<Operation> ParseFilterOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    std::string expr;
    ASSIGN_OR_RETURN(expr, ParseBalancedExpr());
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    FilterOp op;
    op.expr = std::move(expr);
    return Operation{std::move(op)};
  }

  // .select(col1, col2 AS alias, expr AS alias, ...)
  base::StatusOr<Operation> ParseSelectOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    SelectOp op;
    while (!PeekType(TokenType::kRParen)) {
      ColumnSpec col;
      ASSIGN_OR_RETURN(col, ParseColumnSpec());
      op.columns.push_back(std::move(col));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .add_columns(from: ref, on: col = col, cols: [col, ...])
  base::StatusOr<Operation> ParseAddColumnsOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    AddColumnsOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "from")) {
        // Support both "from: ref_name" and "from: table('name')".
        if (PeekIdent("table")) {
          tok_.Next();  // consume "table"
          RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
          Token tname;
          ASSIGN_OR_RETURN(tname, Expect(TokenType::kString, "table name"));
          RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
          op.from_ref.name = UnquoteString(tname.text);
          // Check for optional alias.
          if (PeekIdent("AS")) {
            tok_.Next();
            Token alias_tok = tok_.Next();
            op.from_ref.alias = std::string(alias_tok.text);
          }
        } else {
          ASSIGN_OR_RETURN(op.from_ref, ParsePipelineRef());
        }
      } else if (IdentIs(key.text, "on")) {
        std::string cond;
        ASSIGN_OR_RETURN(cond, ParseBalancedExpr());
        auto eq_pos = cond.find(" = ");
          bool has_complex = cond.find("BETWEEN") != std::string::npos || cond.find(" AND ") != std::string::npos || cond.find(" OR ") != std::string::npos;
        if (!has_complex && eq_pos != std::string::npos) {
          op.on_left_col = cond.substr(0, eq_pos);
          op.on_right_col = cond.substr(eq_pos + 3);
        } else {
          op.on_expr = cond;
        }
      } else if (IdentIs(key.text, "cols")) {
        RETURN_IF_ERROR(Expect(TokenType::kLBracket, "'['").status());
        while (!PeekType(TokenType::kRBracket)) {
          ColumnSpec col;
          ASSIGN_OR_RETURN(col, ParseColumnSpec());
          op.columns.push_back(std::move(col));
          if (PeekType(TokenType::kComma))
            tok_.Next();
        }
        RETURN_IF_ERROR(Expect(TokenType::kRBracket, "']'").status());
      } else {
        return ErrorAt(key, "unknown add_columns parameter");
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .group_by(col1, col2).agg(name: func(col), ...)
  base::StatusOr<Operation> ParseGroupByOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    GroupByOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token col = tok_.Next();
      if (col.type != TokenType::kIdent) {
        return ErrorAt(col, "expected column name in group_by");
      }
      std::string col_name(col.text);
      // Handle dotted column names (e.g., "table.column").
      while (PeekType(TokenType::kDot)) {
        tok_.Next();  // consume dot
        Token part = tok_.Next();
        col_name += "." + std::string(part.text);
      }
      op.columns.push_back(std::move(col_name));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());

    // Check for chained .agg().
    if (tok_.Peek().type == TokenType::kDot) {
      // Peek ahead to see if it's .agg.
      tok_.Next();  // consume dot
      if (PeekIdent("agg")) {
        tok_.Next();  // consume "agg"
        RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
        while (!PeekType(TokenType::kRParen)) {
          AggSpec agg;
          ASSIGN_OR_RETURN(agg, ParseAggSpec());
          op.aggregations.push_back(std::move(agg));
          if (PeekType(TokenType::kComma))
            tok_.Next();
        }
        RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
      } else {
        // It's not .agg, but some other operation. We need to "unpeek" the dot.
        // This is tricky... Let's handle it by returning the GroupByOp and
        // letting the caller handle the remaining .op.
        // We can't easily put the dot back, so we'll just accept group_by
        // without agg and handle the dot+next_op in the caller.
        // Actually, we consumed the dot already. We need to handle this.
        // The simplest fix: peek at the token after dot before consuming.
        // For now, return an error - group_by must be followed by agg or end.
        Token next = tok_.Peek();
        return ErrorAt(next,
                       ".group_by() must be followed by .agg(), not ." +
                           std::string(next.text));
      }
    }

    return Operation{std::move(op)};
  }

  // Parse a single aggregation spec: "name: func(col)" or "name: func()"
  base::StatusOr<AggSpec> ParseAggSpec() {
    AggSpec spec;
    Token name;
    ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "aggregation name"));
    spec.result_name = std::string(name.text);
    RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());

    // After the colon, parse either:
    //   func(col)        — standard aggregation
    //   func(col, arg)   — aggregation with extra arg (percentile)
    //   expr             — raw expression (for pass-through columns)
    // Peek ahead: if we see IDENT followed by '(', it's a function call.
    // Otherwise, collect as a raw expression.
    Token first = tok_.Peek();
    if (first.type == TokenType::kIdent) {
      tok_.Next();
      if (PeekType(TokenType::kLParen)) {
        // Function call: func(...). Collect the entire call as a custom_expr
        // to handle complex args like sum(iif(state = 'Running', dur, 0)).
        std::string call_expr(first.text);
        // Collect everything from ( to matching ) as balanced expression.
        tok_.Next();  // consume (
        call_expr += "(";
        std::string args;
        ASSIGN_OR_RETURN(args, ParseBalancedExpr());
        call_expr += args;
        RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
        call_expr += ")";
        // Check for trailing expression (e.g., "/ 1e3").
        std::string trailing;
        ASSIGN_OR_RETURN(trailing, ParseBalancedExpr());
        if (!trailing.empty()) {
          call_expr += " " + trailing;
        }
        spec.custom_expr = call_expr;
      } else {
        // Raw expression: collect everything until comma or rparen.
        std::string expr(first.text);
        std::string rest;
        ASSIGN_OR_RETURN(rest, ParseBalancedExpr());
        if (!rest.empty()) {
          expr += " " + rest;
        }
        spec.custom_expr = expr;
      }
    } else {
      // Raw expression starting with non-ident.
      std::string expr;
      ASSIGN_OR_RETURN(expr, ParseBalancedExpr());
      spec.custom_expr = expr;
    }
    return spec;
  }

  // .sort(col1 DESC, col2 ASC, col3)
  base::StatusOr<Operation> ParseSortOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    SortOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token col = tok_.Next();
      if (col.type != TokenType::kIdent) {
        return ErrorAt(col, "expected column name in sort");
      }
      SortSpec spec;
      spec.column = std::string(col.text);
      if (PeekIdent("DESC")) {
        tok_.Next();
        spec.desc = true;
      } else if (PeekIdent("ASC")) {
        tok_.Next();
        spec.desc = false;
      }
      op.specs.push_back(std::move(spec));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .limit(N)
  base::StatusOr<Operation> ParseLimitOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    Token val;
    ASSIGN_OR_RETURN(val, Expect(TokenType::kInt, "integer"));
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    LimitOp op;
    op.limit = std::stoll(std::string(val.text));
    return Operation{std::move(op)};
  }

  // .offset(N)
  base::StatusOr<Operation> ParseOffsetOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    Token val;
    ASSIGN_OR_RETURN(val, Expect(TokenType::kInt, "integer"));
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    OffsetOp op;
    op.offset = std::stoll(std::string(val.text));
    return Operation{std::move(op)};
  }

  // .filter_during(ref, partition: [...], clip: true/false)
  base::StatusOr<Operation> ParseFilterDuringOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    FilterDuringOp op;
    ASSIGN_OR_RETURN(op.intervals, ParsePipelineRef());
    while (PeekType(TokenType::kComma)) {
      tok_.Next();
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "partition")) {
        ASSIGN_OR_RETURN(op.partition_columns, ParseBracketedList());
      } else if (IdentIs(key.text, "clip")) {
        Token val = tok_.Next();
        op.clip = IdentIs(val.text, "true");
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .filter_in(match_ref, base_col: col, match_col: col)
  base::StatusOr<Operation> ParseFilterInOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    FilterInOp op;
    ASSIGN_OR_RETURN(op.match_ref, ParsePipelineRef());
    while (PeekType(TokenType::kComma)) {
      tok_.Next();
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      Token val = tok_.Next();
      if (IdentIs(key.text, "base_col")) {
        op.base_column = std::string(val.text);
      } else if (IdentIs(key.text, "match_col")) {
        op.match_column = std::string(val.text);
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // Parse a column spec: "name", "name AS alias", "expr AS alias".
  base::StatusOr<ColumnSpec> ParseColumnSpec() {
    // Collect tokens until comma, rparen, or rbracket.
    std::string expr;
    std::string alias;
    int depth = 0;

    while (true) {
      Token t = tok_.Peek();
      if (t.type == TokenType::kEof)
        break;
      if (depth == 0 &&
          (t.type == TokenType::kComma || t.type == TokenType::kRParen ||
           t.type == TokenType::kRBracket)) {
        break;
      }
      if (t.type == TokenType::kLParen)
        ++depth;
      if (t.type == TokenType::kRParen)
        --depth;

      // Check for AS keyword at depth 0.
      if (depth == 0 && t.type == TokenType::kIdent && IdentIs(t.text, "AS")) {
        tok_.Next();  // consume AS
        Token alias_tok = tok_.Next();
        alias = std::string(alias_tok.text);
        break;
      }

      tok_.Next();
      if (!expr.empty())
        expr += " ";
      expr += t.text;
    }

    ColumnSpec spec;
    spec.expr = std::move(expr);
    spec.alias = std::move(alias);
    return spec;
  }

  // @function name(param: TYPE, ...) -> RETURN_TYPE : pipeline_body
  // @function name(param: TYPE, ...) -> TABLE(col: TYPE, ...) : pipeline_body
  base::StatusOr<FunctionDecl> ParseFunctionDecl(uint32_t line) {
    FunctionDecl decl;
    decl.line = line;

    // Parse function name.
    Token name;
    ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "function name"));
    decl.name = std::string(name.text);

    // Parse parameter list.
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    while (!PeekType(TokenType::kRParen)) {
      FunctionParam param;
      Token param_name;
      ASSIGN_OR_RETURN(param_name,
                       Expect(TokenType::kIdent, "parameter name"));
      param.name = std::string(param_name.text);
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      Token param_type;
      ASSIGN_OR_RETURN(param_type,
                       Expect(TokenType::kIdent, "parameter type"));
      param.type = std::string(param_type.text);
      decl.params.push_back(std::move(param));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());

    // Parse -> (as - then >) for return type.
    RETURN_IF_ERROR(Expect(TokenType::kMinus, "'-'").status());
    RETURN_IF_ERROR(Expect(TokenType::kGreater, "'>'").status());

    // Check if return type is TABLE(...) or a simple type.
    if (PeekIdent("TABLE")) {
      tok_.Next();  // consume "TABLE"
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      while (!PeekType(TokenType::kRParen)) {
        FunctionReturnCol col;
        Token col_name;
        ASSIGN_OR_RETURN(col_name, Expect(TokenType::kIdent, "column name"));
        col.name = std::string(col_name.text);
        RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
        Token col_type;
        ASSIGN_OR_RETURN(col_type, Expect(TokenType::kIdent, "column type"));
        col.type = std::string(col_type.text);
        decl.return_cols.push_back(std::move(col));
        if (PeekType(TokenType::kComma))
          tok_.Next();
      }
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    } else {
      Token ret_type;
      ASSIGN_OR_RETURN(ret_type, Expect(TokenType::kIdent, "return type"));
      decl.return_type = std::string(ret_type.text);
    }

    // Parse colon before body.
    RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());

    // Body is either sql(...) for a raw SQL expression, or a pipeline.
    if (PeekIdent("sql")) {
      // Raw SQL body: sql('...')
      tok_.Next();  // consume "sql"
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      Token sql_str;
      ASSIGN_OR_RETURN(sql_str, Expect(TokenType::kString, "SQL string"));
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
      decl.sql_body = UnquoteString(sql_str.text);
    } else {
      // Pipeline body: source + operations.
      Pipeline pipeline;
      ASSIGN_OR_RETURN(pipeline.source, ParseSource());
      while (tok_.Peek().type == TokenType::kDot) {
        tok_.Next();  // consume dot
        Operation op;
        ASSIGN_OR_RETURN(op, ParseOperation());
        pipeline.operations.push_back(std::move(op));
      }
      decl.pipeline_body = std::move(pipeline);
    }

    return decl;
  }

  // Like ParseBalancedExpr but also stops at a given keyword (case-insensitive)
  // at paren depth 0. Does NOT consume the keyword.
  base::StatusOr<std::string> ParseBalancedExprUntilKeyword(
      const char* keyword) {
    std::string result;
    int paren_depth = 0;
    int bracket_depth = 0;

    while (true) {
      Token t = tok_.Peek();
      if (t.type == TokenType::kEof)
        break;

      if (paren_depth == 0 && bracket_depth == 0) {
        if (t.type == TokenType::kRParen || t.type == TokenType::kRBracket)
          break;
        if (t.type == TokenType::kComma)
          break;
        if (t.type == TokenType::kIdent && IdentIs(t.text, keyword))
          break;
      }

      tok_.Next();
      if (t.type == TokenType::kLParen)
        ++paren_depth;
      if (t.type == TokenType::kRParen)
        --paren_depth;
      if (t.type == TokenType::kLBracket)
        ++bracket_depth;
      if (t.type == TokenType::kRBracket)
        --bracket_depth;

      if (!result.empty())
        result += " ";
      result += t.text;
    }

    return result;
  }

  // .window(name: func(col) over (partition: [cols], order: col), ...)
  base::StatusOr<Operation> ParseWindowOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    WindowOp op;
    while (!PeekType(TokenType::kRParen)) {
      WindowSpec spec;

      // Parse "result_name:"
      Token name;
      ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "window spec name"));
      spec.result_name = std::string(name.text);
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());

      // Parse function expression tokens until "over" keyword.
      std::string func_expr;
      ASSIGN_OR_RETURN(func_expr, ParseBalancedExprUntilKeyword("over"));
      spec.func_expr = std::move(func_expr);

      // Expect "over" keyword.
      if (!PeekIdent("over")) {
        return ErrorAt(tok_.Peek(),
                       "expected 'over' keyword in window spec");
      }
      tok_.Next();  // consume "over"

      // Parse over clause: (partition: [...], order: col [DESC|ASC],
      //                     frame: 'sql')
      RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
      while (!PeekType(TokenType::kRParen)) {
        Token key;
        ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "over clause key"));
        RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
        if (IdentIs(key.text, "partition")) {
          ASSIGN_OR_RETURN(spec.partition, ParseBracketedList());
        } else if (IdentIs(key.text, "order")) {
          // Collect the full order expression (may include commas for
          // multi-column ORDER BY). Stop at ')' or next kwarg ('frame:').
          std::string expr;
          int depth = 0;
          while (true) {
            Token t = tok_.Peek();
            if (t.type == TokenType::kEof) break;
            if (depth == 0 && t.type == TokenType::kRParen) break;
            // Stop at next kwarg: ident followed by ':'
            if (depth == 0 && t.type == TokenType::kIdent &&
                IdentIs(t.text, "frame")) break;
            tok_.Next();
            if (t.type == TokenType::kLParen) ++depth;
            if (t.type == TokenType::kRParen) --depth;
            if (!expr.empty()) expr += " ";
            expr += t.text;
          }
          spec.order_expr = std::move(expr);
        } else if (IdentIs(key.text, "frame")) {
          Token val;
          ASSIGN_OR_RETURN(val, Expect(TokenType::kString, "frame spec"));
          spec.frame = UnquoteString(val.text);
        } else {
          return ErrorAt(key, "unknown window over parameter: " +
                                  std::string(key.text));
        }
        if (PeekType(TokenType::kComma))
          tok_.Next();
      }
      RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());

      op.specs.push_back(std::move(spec));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .computed(name: expr, ...)
  base::StatusOr<Operation> ParseComputedOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    ComputedOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token name;
      ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "computed column name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      std::string expr;
      ASSIGN_OR_RETURN(expr, ParseBalancedExpr());
      ColumnSpec col;
      col.expr = std::move(expr);
      col.alias = std::string(name.text);
      op.columns.push_back(std::move(col));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .classify(result_col, from: source_col, 'pattern' => 'value', ...)
  base::StatusOr<Operation> ParseClassifyOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    ClassifyOp op;

    // Parse result column name.
    Token result_col;
    ASSIGN_OR_RETURN(result_col,
                     Expect(TokenType::kIdent, "result column name"));
    op.result_column = std::string(result_col.text);
    RETURN_IF_ERROR(Expect(TokenType::kComma, "','").status());

    // Parse "from: source_col".
    if (!PeekIdent("from")) {
      return ErrorAt(tok_.Peek(), "expected 'from:' in classify");
    }
    tok_.Next();  // consume "from"
    RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
    Token source_col;
    ASSIGN_OR_RETURN(source_col,
                     Expect(TokenType::kIdent, "source column name"));
    op.source_column = std::string(source_col.text);

    // Parse comma-separated mappings: 'pattern' => 'value' or _ => 'value'
    while (PeekType(TokenType::kComma)) {
      tok_.Next();  // consume comma
      ClassifyMapping mapping;
      Token pattern_tok = tok_.Peek();
      if (pattern_tok.type == TokenType::kString) {
        tok_.Next();
        mapping.pattern = UnquoteString(pattern_tok.text);
        mapping.is_default = false;
      } else if (pattern_tok.type == TokenType::kIdent &&
                 IdentIs(pattern_tok.text, "_")) {
        tok_.Next();
        mapping.is_default = true;
      } else {
        return ErrorAt(pattern_tok,
                       "expected string pattern or '_' in classify mapping");
      }

      // Parse => as = then >.
      RETURN_IF_ERROR(Expect(TokenType::kEquals, "'='").status());
      RETURN_IF_ERROR(Expect(TokenType::kGreater, "'>'").status());

      Token value_tok;
      ASSIGN_OR_RETURN(value_tok, Expect(TokenType::kString, "mapping value"));
      mapping.value = UnquoteString(value_tok.text);
      op.mappings.push_back(std::move(mapping));
    }

    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .extract_args(name: 'path', ...)
  base::StatusOr<Operation> ParseExtractArgsOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    ExtractArgsOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token name;
      ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "extraction name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      Token path;
      ASSIGN_OR_RETURN(path, Expect(TokenType::kString, "arg path string"));
      op.extractions.emplace_back(std::string(name.text),
                                  UnquoteString(path.text));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .flow_reachable(direction: 'out')
  base::StatusOr<Operation> ParseFlowReachableOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    FlowReachableOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "direction")) {
        Token val = tok_.Next();
        op.direction = UnquoteString(val.text);
      } else {
        return ErrorAt(key, "unknown flow_reachable parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .merge_overlapping(epsilon: N, partition: [cols])
  base::StatusOr<Operation> ParseMergeOverlappingOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    MergeOverlappingOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "epsilon")) {
        Token val = tok_.Next();
        op.epsilon = std::stoll(std::string(val.text));
      } else if (IdentIs(key.text, "partition")) {
        ASSIGN_OR_RETURN(op.partition_columns, ParseBracketedList());
      } else {
        return ErrorAt(key, "unknown merge_overlapping parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .graph_reachable(edges_ref, method: 'dfs')
  base::StatusOr<Operation> ParseGraphReachableOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    GraphReachableOp op;
    ASSIGN_OR_RETURN(op.edges, ParsePipelineRef());
    while (PeekType(TokenType::kComma)) {
      tok_.Next();
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "method")) {
        Token val = tok_.Next();
        op.method = UnquoteString(val.text);
      } else {
        return ErrorAt(key, "unknown graph_reachable parameter: " +
                                std::string(key.text));
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .except(ref)
  base::StatusOr<Operation> ParseExceptOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    ExceptOp op;
    ASSIGN_OR_RETURN(op.other, ParsePipelineRef());
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .span_join(ref, partition: [...], type: LEFT)
  base::StatusOr<Operation> ParseSpanJoinOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    SpanJoinOp op;
    ASSIGN_OR_RETURN(op.right, ParsePipelineRef());
    while (PeekType(TokenType::kComma)) {
      tok_.Next();
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "partition")) {
        ASSIGN_OR_RETURN(op.partition_columns, ParseBracketedList());
      } else if (IdentIs(key.text, "type")) {
        Token val = tok_.Next();
        op.is_left = IdentIs(val.text, "LEFT");
      } else {
        return ErrorAt(key, "unknown span_join parameter: " +
                                std::string(key.text));
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .unpivot(value_col: name, name_col: name, columns: [col1, col2, ...])
  base::StatusOr<Operation> ParseUnpivotOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    UnpivotOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "value_col")) {
        Token val = tok_.Next();
        op.value_column = std::string(val.text);
      } else if (IdentIs(key.text, "name_col")) {
        Token val = tok_.Next();
        op.name_column = std::string(val.text);
      } else if (IdentIs(key.text, "columns")) {
        ASSIGN_OR_RETURN(op.source_columns, ParseBracketedList());
      } else {
        return ErrorAt(key, "unknown unpivot parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .index(col1, col2, ...)
  base::StatusOr<Operation> ParseIndexOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    IndexOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token col = tok_.Next();
      if (col.type != TokenType::kIdent) {
        return ErrorAt(col, "expected column name in index");
      }
      op.columns.push_back(std::string(col.text));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .parse_name('template{field1}sep{field2}...')
  base::StatusOr<Operation> ParseParseNameOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    Token tmpl_str;
    ASSIGN_OR_RETURN(tmpl_str, Expect(TokenType::kString, "template string"));
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    ParseNameOp op;
    op.template_str = UnquoteString(tmpl_str.text);
    return Operation{std::move(op)};
  }

  // .closest_preceding(other, match: col = col, order: ts)
  base::StatusOr<Operation> ParseClosestPrecedingOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    ClosestPrecedingOp op;
    ASSIGN_OR_RETURN(op.other, ParsePipelineRef());
    while (PeekType(TokenType::kComma)) {
      tok_.Next();  // consume comma
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "match")) {
        std::string cond;
        ASSIGN_OR_RETURN(cond, ParseBalancedExpr());
        auto eq_pos = cond.find(" = ");
          bool has_complex = cond.find("BETWEEN") != std::string::npos || cond.find(" AND ") != std::string::npos || cond.find(" OR ") != std::string::npos;
        if (!has_complex && eq_pos != std::string::npos) {
          op.match_left_col = cond.substr(0, eq_pos);
          op.match_right_col = cond.substr(eq_pos + 3);
          // Trim whitespace.
          while (!op.match_left_col.empty() &&
                 op.match_left_col.back() == ' ')
            op.match_left_col.pop_back();
          while (!op.match_right_col.empty() &&
                 op.match_right_col.front() == ' ')
            op.match_right_col.erase(op.match_right_col.begin());
        } else {
          return ErrorAt(key,
                         "match: expects 'left_col = right_col' format");
        }
      } else if (IdentIs(key.text, "order")) {
        Token val = tok_.Next();
        op.order_expr = std::string(val.text);
      } else {
        return ErrorAt(key, "unknown closest_preceding parameter: " +
                                std::string(key.text));
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .find_ancestor(where: expr, cols: [col AS alias, ...])
  base::StatusOr<Operation> ParseFindAncestorOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    FindAncestorOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "where")) {
        std::string expr;
        ASSIGN_OR_RETURN(expr, ParseBalancedExpr());
        op.where_expr = std::move(expr);
      } else if (IdentIs(key.text, "cols")) {
        RETURN_IF_ERROR(Expect(TokenType::kLBracket, "'['").status());
        while (!PeekType(TokenType::kRBracket)) {
          ColumnSpec col;
          ASSIGN_OR_RETURN(col, ParseColumnSpec());
          op.columns.push_back(std::move(col));
          if (PeekType(TokenType::kComma))
            tok_.Next();
        }
        RETURN_IF_ERROR(Expect(TokenType::kRBracket, "']'").status());
      } else {
        return ErrorAt(key, "unknown find_ancestor parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .find_descendant(where: expr, cols: [col AS alias, ...])
  base::StatusOr<Operation> ParseFindDescendantOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    FindDescendantOp op;
    while (!PeekType(TokenType::kRParen)) {
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "where")) {
        std::string expr;
        ASSIGN_OR_RETURN(expr, ParseBalancedExpr());
        op.where_expr = std::move(expr);
      } else if (IdentIs(key.text, "cols")) {
        RETURN_IF_ERROR(Expect(TokenType::kLBracket, "'['").status());
        while (!PeekType(TokenType::kRBracket)) {
          ColumnSpec col;
          ASSIGN_OR_RETURN(col, ParseColumnSpec());
          op.columns.push_back(std::move(col));
          if (PeekType(TokenType::kComma))
            tok_.Next();
        }
        RETURN_IF_ERROR(Expect(TokenType::kRBracket, "']'").status());
      } else {
        return ErrorAt(key, "unknown find_descendant parameter: " +
                                std::string(key.text));
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .join(right_ref, on: expr, type: LEFT|INNER)
  base::StatusOr<Operation> ParseJoinOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    JoinOp op;
    ASSIGN_OR_RETURN(op.right, ParsePipelineRef());
    while (PeekType(TokenType::kComma)) {
      tok_.Next();  // consume comma
      Token key;
      ASSIGN_OR_RETURN(key, Expect(TokenType::kIdent, "parameter name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      if (IdentIs(key.text, "on")) {
        std::string expr;
        ASSIGN_OR_RETURN(expr, ParseBalancedExpr());
        op.on_expr = std::move(expr);
      } else if (IdentIs(key.text, "type")) {
        Token val = tok_.Next();
        if (IdentIs(val.text, "LEFT")) {
          op.is_left = true;
        } else if (IdentIs(val.text, "INNER")) {
          op.is_left = false;
        } else {
          return ErrorAt(val, "expected LEFT or INNER for join type");
        }
      } else {
        return ErrorAt(key,
                       "unknown join parameter: " + std::string(key.text));
      }
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // .cross_join(right_ref)
  base::StatusOr<Operation> ParseCrossJoinOp() {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    CrossJoinOp op;
    ASSIGN_OR_RETURN(op.right, ParsePipelineRef());
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // Template call as operation: .template_name(name: value, ...)
  // Used as fallback for unknown operation names.
  base::StatusOr<Operation> ParseTemplateCallOp(std::string name) {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    TemplateCallOp op;
    op.template_name = std::move(name);
    while (!PeekType(TokenType::kRParen)) {
      Token arg_name;
      ASSIGN_OR_RETURN(arg_name, Expect(TokenType::kIdent, "argument name"));
      RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());
      std::string arg_value;
      ASSIGN_OR_RETURN(arg_value, ParseBalancedExpr());
      op.args.emplace_back(std::string(arg_name.text), std::move(arg_value));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Operation{std::move(op)};
  }

  // lookup_table('key1' => val1, 'key2' => val2, ...)
  base::StatusOr<Source> ParseLookupTableSource() {
    tok_.Next();  // consume "lookup_table"
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    LookupTableSource src;
    while (!PeekType(TokenType::kRParen)) {
      Token key_tok;
      ASSIGN_OR_RETURN(key_tok, Expect(TokenType::kString, "lookup key string"));
      std::string key = UnquoteString(key_tok.text);
      // Parse => as '=' then '>'.
      RETURN_IF_ERROR(Expect(TokenType::kEquals, "'='").status());
      RETURN_IF_ERROR(Expect(TokenType::kGreater, "'>'").status());
      // Value can be a string or int.
      Token val_tok = tok_.Next();
      std::string value;
      if (val_tok.type == TokenType::kString) {
        value = UnquoteString(val_tok.text);
      } else if (val_tok.type == TokenType::kInt) {
        value = std::string(val_tok.text);
      } else {
        return ErrorAt(val_tok,
                       "expected string or integer value in lookup_table");
      }
      src.entries.emplace_back(std::move(key), std::move(value));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  // Template call as source: template_name(arg1, name: value, ...)
  // Name already consumed by caller.
  base::StatusOr<Source> ParseTemplateCallSource(std::string name) {
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    TemplateCallSource src;
    src.template_name = std::move(name);
    while (!PeekType(TokenType::kRParen)) {
      Token first = tok_.Peek();
      // Check if this is a named arg (IDENT followed by ':').
      if (first.type == TokenType::kIdent) {
        // Save position and check if next is colon.
        tok_.Next();  // consume ident
        if (PeekType(TokenType::kColon)) {
          tok_.Next();  // consume colon
          std::string arg_value;
          ASSIGN_OR_RETURN(arg_value, ParseBalancedExpr());
          src.args.emplace_back(std::string(first.text),
                                std::move(arg_value));
        } else {
          // Not a named arg; this ident is a positional value.
          src.args.emplace_back("", std::string(first.text));
        }
      } else if (first.type == TokenType::kString) {
        tok_.Next();
        // Keep single quotes for SQL substitution.
        std::string val = UnquoteString(first.text);
        src.args.emplace_back("", "'" + val + "'");
      } else if (first.type == TokenType::kInt) {
        tok_.Next();
        src.args.emplace_back("", std::string(first.text));
      } else {
        return ErrorAt(first,
                       "expected argument in template call source");
      }
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());
    return Source{std::move(src)};
  }

  // @define template_name(param: Type, ...) : body
  base::StatusOr<TemplateDecl> ParseTemplateDecl(uint32_t line) {
    TemplateDecl decl;
    decl.line = line;

    // Parse template name.
    Token name;
    ASSIGN_OR_RETURN(name, Expect(TokenType::kIdent, "template name"));
    decl.name = std::string(name.text);

    // Parse parameter list. Type annotation is optional (defaults to Pipeline).
    RETURN_IF_ERROR(Expect(TokenType::kLParen, "'('").status());
    while (!PeekType(TokenType::kRParen)) {
      TemplateParam param;
      Token param_name;
      ASSIGN_OR_RETURN(param_name,
                       Expect(TokenType::kIdent, "parameter name"));
      param.name = std::string(param_name.text);
      if (PeekType(TokenType::kColon)) {
        tok_.Next();  // consume ':'
        Token param_type;
        ASSIGN_OR_RETURN(param_type,
                         Expect(TokenType::kIdent, "parameter type"));
        param.type = std::string(param_type.text);
      } else {
        param.type = "Pipeline";
      }
      decl.params.push_back(std::move(param));
      if (PeekType(TokenType::kComma))
        tok_.Next();
    }
    RETURN_IF_ERROR(Expect(TokenType::kRParen, "')'").status());

    // Parse colon before body.
    RETURN_IF_ERROR(Expect(TokenType::kColon, "':'").status());

    // Body: if next token is '.', this is an operation-only template.
    // Otherwise it's a full pipeline (source + operations).
    if (PeekType(TokenType::kDot)) {
      decl.is_operation = true;
      // Operation-only template: parse a dummy source (won't be used).
      decl.body.source = PipelineRef{};
      while (tok_.Peek().type == TokenType::kDot) {
        tok_.Next();  // consume dot
        Operation op;
        ASSIGN_OR_RETURN(op, ParseOperation());
        decl.body.operations.push_back(std::move(op));
      }
    } else {
      decl.is_operation = false;
      ASSIGN_OR_RETURN(decl.body.source, ParseSource());
      while (tok_.Peek().type == TokenType::kDot) {
        tok_.Next();  // consume dot
        Operation op;
        ASSIGN_OR_RETURN(op, ParseOperation());
        decl.body.operations.push_back(std::move(op));
      }
    }

    return decl;
  }

  // Collect tokens until we hit an unmatched comma, rparen, or specific keyword
  // that signals end of expression. Returns the collected text as a string.
  // Does NOT consume the terminating token.
  base::StatusOr<std::string> ParseBalancedExpr() {
    std::string result;
    int paren_depth = 0;
    int bracket_depth = 0;

    while (true) {
      Token t = tok_.Peek();
      if (t.type == TokenType::kEof)
        break;

      // At depth 0, stop at comma or closing delimiters.
      if (paren_depth == 0 && bracket_depth == 0) {
        if (t.type == TokenType::kRParen || t.type == TokenType::kRBracket)
          break;
        // Stop at comma only if it's a keyword arg separator.
        if (t.type == TokenType::kComma) {
          // Peek further to see if next is "keyword:".
          // For now, just stop at comma for balanced expressions.
          break;
        }
      }

      tok_.Next();
      if (t.type == TokenType::kLParen)
        ++paren_depth;
      if (t.type == TokenType::kRParen)
        --paren_depth;
      if (t.type == TokenType::kLBracket)
        ++bracket_depth;
      if (t.type == TokenType::kRBracket)
        --bracket_depth;

      if (!result.empty())
        result += " ";
      result += t.text;
    }

    return result;
  }

  PfGraphTokenizer tok_;
};

}  // namespace

base::StatusOr<GraphModule> ParsePfGraph(std::string_view input) {
  Parser parser(input);
  return parser.Parse();
}

}  // namespace perfetto::trace_processor::pfgraph
