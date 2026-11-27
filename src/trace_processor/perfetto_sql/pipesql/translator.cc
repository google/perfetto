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

#include "src/trace_processor/perfetto_sql/pipesql/translator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipesql/stages.h"

namespace perfetto::trace_processor::pipesql {

using pipeline::Builder;
using pipeline::ColumnId;
using pipeline::NamedColumn;
using pipeline::NameId;
using pipeline::PlanType;
using pipeline::PreparedStatement;

Catalog::~Catalog() = default;

std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  // syntaqlite has no text for an empty span, such as the name `""`.
  if (span.length == 0) {
    return "";
  }
  uint32_t len;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  PERFETTO_CHECK(text != nullptr);
  std::string name(text, len);
  char quote = syntaqlite_span_quote_char(span);
  if (quote == 0 || quote == '[') {
    return name;
  }
  return base::ReplaceAll(name, std::string(2, quote), std::string(1, quote));
}

bool IsSpanPresent(SyntaqliteTextSpan span) {
  return span.length != 0 || syntaqlite_span_is_quoted(span);
}

Translator::Translator(SyntaqliteParser* p,
                       const NodeSourceFn& source,
                       const Catalog& c)
    : p_(p), source_(source), catalog_(c) {}

base::StatusOr<ColumnId> Translator::Resolve(NameId name, uint32_t at) const {
  return Resolve(std::nullopt, name, at);
}

base::Status Translator::Unsupported(uint32_t at, std::string_view what) const {
  return Err(at, Error::kUnsupported, what);
}

base::Status Translator::Expected(uint32_t at, std::string_view what) const {
  return Err(at, Error::kExpected, what);
}

void Translator::Append(NamedColumn column,
                        uint32_t node,
                        bool qualified_only) {
  scope_.row.push_back({column, scope_.operation, node, qualified_only});
}

SqlSource Translator::SourceForNode(uint32_t node) const {
  return source_(node);
}

void Translator::SetOperation(const char* operation) {
  scope_.operation = operation;
}

void Translator::ReplaceRow(std::vector<RowColumn> row) {
  scope_.row = std::move(row);
}

void Translator::ExtendRow(std::vector<RowColumn> columns) {
  scope_.row.insert(scope_.row.end(), columns.begin(), columns.end());
}

void Translator::DropColumn(NameId name) {
  scope_.row.erase(std::remove_if(scope_.row.begin(), scope_.row.end(),
                                  [&](const RowColumn& column) {
                                    return !column.qualified_only &&
                                           Same(column.column.name, name);
                                  }),
                   scope_.row.end());
  RemoveAlias(name);
}

void Translator::RenameColumn(size_t at, NameId name, uint32_t node) {
  RowColumn& column = scope_.row[at];
  column.column.name = name;
  column.operation = scope_.operation;
  column.node = node;
}

void Translator::SetColumnValue(size_t at, ColumnId value, uint32_t node) {
  RowColumn& column = scope_.row[at];
  column.column.id = value;
  column.operation = scope_.operation;
  column.node = node;
  RemoveAlias(column.column.name);
}

void Translator::ClearAliases() {
  scope_.aliases.clear();
}

void Translator::AddAlias(Alias alias) {
  scope_.aliases.push_back(std::move(alias));
}

void Translator::AddAlias(NameId name, size_t first_column) {
  const std::vector<RowColumn>& row = scope_.row;
  AddAlias(
      {name,
       {row.begin() + static_cast<std::ptrdiff_t>(first_column), row.end()}});
}

void Translator::ReplaceAliasColumns(
    const std::vector<std::pair<ColumnId, ColumnId>>& replaced) {
  for (Alias& alias : scope_.aliases) {
    std::vector<RowColumn> columns;
    for (RowColumn column : alias.columns) {
      auto it = std::find_if(
          replaced.begin(), replaced.end(),
          [&](const auto& pair) { return pair.first == column.column.id; });
      if (it != replaced.end()) {
        column.column.id = it->second;
        columns.push_back(std::move(column));
      }
    }
    alias.columns = std::move(columns);
  }
}

base::Status Translator::TranslateStage(uint32_t stage) {
  uint32_t tag = Node<SyntaqliteNode>(p_, stage)->tag;
  std::string unsupported;
  switch (tag) {
    case SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE:
      return TranslateFrom(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_SELECT:
      return TranslateSelect(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_EXTEND:
      return TranslateExtend(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_DROP:
      return TranslateDrop(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_RENAME:
      return TranslateRename(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_SET:
      return TranslateSet(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_AS:
      return TranslateAs(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_WHERE:
      return TranslateWhere(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_AGGREGATE:
      return TranslateAggregate(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_DISTINCT:
      return TranslateDistinct(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_JOIN:
      return TranslateJoin(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_ORDER_BY:
      return TranslateOrderBy(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_LIMIT:
      return TranslateLimit(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE:
      return TranslateTreeAccumulate(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_TREE_KEY_PATHS:
      return TranslateTreeKeyPaths(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_GRAPH_TREE:
      return TranslateGraphTree(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_INTERVAL_INTERSECTION:
      return TranslateIntervalIntersect(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_INTERVAL_FLATTEN:
      return TranslateIntervalFlatten(this, stage);
    case SYNTAQLITE_NODE_PERFETTO_INTERVAL_FILL_GAPS:
      return TranslateIntervalFillGaps(this, stage);
    // Grammar-accepted stages that do not yet have execution support.
    case SYNTAQLITE_NODE_PERFETTO_INTERVAL_MERGE:
      unsupported = "INTERVAL MERGE";
      break;
    case SYNTAQLITE_NODE_PERFETTO_INTERVALS_FROM_EVENTS:
      unsupported = "INTERVALS FROM EVENTS";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_FIND: {
      const auto* find = Node<SyntaqlitePerfettoTreeFind>(p_, stage);
      if (find->join_type != SYNTAQLITE_JOIN_TYPE_INNER &&
          find->join_type != SYNTAQLITE_JOIN_TYPE_LEFT) {
        return source_(stage).AddTraceback(
            0, base::ErrStatus("TREE FIND can only be preceded by LEFT"));
      }
      unsupported = "TREE FIND";
      break;
    }
    case SYNTAQLITE_NODE_PERFETTO_TREE_FOLD:
      unsupported = Node<SyntaqlitePerfettoTreeFold>(p_, stage)->kind ==
                            SYNTAQLITE_PERFETTO_TREE_FOLD_KIND_CONTRACT
                        ? "TREE CONTRACT"
                        : "TREE ABSORB";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_PRUNE:
      unsupported = "TREE PRUNE";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_REPARENT:
      unsupported = "TREE REPARENT";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_MERGE_SIBLINGS:
      unsupported = "TREE MERGE SIBLINGS";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_MERGE_INTO_PARENT:
      unsupported = "TREE MERGE INTO PARENT";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_INVERT:
      unsupported = "TREE INVERT";
      break;
    case SYNTAQLITE_NODE_PERFETTO_TREE_EXPAND:
      unsupported = "TREE EXPAND";
      break;
    default: {
      std::string sql = source_(stage).sql();
      unsupported = sql.substr(0, sql.find_first_of(" \n"));
      break;
    }
  }
  return source_(stage).AddTraceback(
      0, base::ErrStatus("%s is not supported yet", unsupported.c_str()));
}

base::Status Translator::Err(uint32_t at,
                             Error error,
                             std::string_view what,
                             const std::string& detail) const {
  struct Shape {
    const char* prefix;
    const char* suffix;
  };
  static constexpr Shape kShapes[] = {
      {"expected ", ""},
      {"", " is not supported yet"},
      {"no such column: '", "'"},
      {"column '", "' is ambiguous"},
      {"no such table alias: '", "'"},
  };
  const Shape& shape = kShapes[static_cast<size_t>(error)];
  return source_(at).AddTraceback(
      0, base::ErrStatus("%s: %s%.*s%s%s", scope_.operation, shape.prefix,
                         static_cast<int>(what.size()), what.data(),
                         shape.suffix, detail.c_str()));
}

std::string Translator::Origin(const RowColumn& column) const {
  NameId name = column.column.name;
  for (const Alias& alias : scope_.aliases) {
    for (const RowColumn& aliased : alias.columns) {
      if (aliased.column.id == column.column.id &&
          Same(aliased.column.name, name)) {
        return "`" + Text(alias.name) + "." + Text(name) + "`";
      }
    }
  }
  constexpr size_t kMaxLen = 48;
  std::string text = source_(column.node).sql();
  size_t len = std::min(text.find('\n'), kMaxLen);
  if (len < text.size()) {
    text = text.substr(0, len) + "...";
  }
  return "`" + std::string(column.operation) + " " + text + "`";
}

std::string Translator::AmbiguousCandidates(
    const std::vector<const RowColumn*>& matches) const {
  std::string candidates;
  for (const RowColumn* match : matches) {
    candidates += (candidates.empty() ? ": it could be " : " or ");
    candidates += Origin(*match);
  }
  return candidates;
}

const Translator::Alias* Translator::FindAlias(NameId name) const {
  for (const Alias& alias : scope_.aliases) {
    if (Same(alias.name, name)) {
      return &alias;
    }
  }
  return nullptr;
}

std::optional<ColumnId> Translator::FindColumn(
    const std::vector<NamedColumn>& columns,
    NameId name) const {
  for (const NamedColumn& column : columns) {
    if (Same(column.name, name)) {
      return column.id;
    }
  }
  return std::nullopt;
}

void Translator::RemoveAlias(NameId name) {
  scope_.aliases.erase(
      std::remove_if(
          scope_.aliases.begin(), scope_.aliases.end(),
          [&](const Alias& alias) { return Same(alias.name, name); }),
      scope_.aliases.end());
}

base::Status Translator::NoSuchColumn(NameId name, uint32_t at) const {
  if (FindAlias(name)) {
    return Expected(at, "a column, but '" + Text(name) + "' is a table alias");
  }
  return Err(at, Error::kNoSuchColumn, Text(name));
}

base::StatusOr<size_t> Translator::FindInRow(NameId name, uint32_t at) const {
  std::vector<const RowColumn*> matches;
  size_t found = 0;
  for (size_t i = 0; i < scope_.row.size(); ++i) {
    if (!scope_.row[i].qualified_only &&
        Same(scope_.row[i].column.name, name)) {
      matches.push_back(&scope_.row[i]);
      found = i;
    }
  }
  if (matches.empty()) {
    return NoSuchColumn(name, at);
  }
  if (matches.size() > 1) {
    return Err(at, Error::kAmbiguousColumn, Text(name),
               AmbiguousCandidates(matches));
  }
  return found;
}

base::Status Translator::CheckListedOnce(std::vector<NameId>& seen,
                                         NameId name,
                                         uint32_t at) const {
  for (NameId other : seen) {
    if (Same(other, name)) {
      return Expected(at, "'" + Text(name) + "' to be listed only once");
    }
  }
  seen.push_back(name);
  return base::OkStatus();
}

base::StatusOr<ColumnId> Translator::Resolve(std::optional<NameId> qualifier,
                                             NameId name,
                                             uint32_t at) const {
  if (!qualifier) {
    ASSIGN_OR_RETURN(size_t i, FindInRow(name, at));
    return scope_.row[i].column.id;
  }
  const Alias* alias = FindAlias(*qualifier);
  std::vector<const RowColumn*> matches;
  if (alias) {
    for (const RowColumn& column : alias->columns) {
      if (Same(column.column.name, name)) {
        matches.push_back(&column);
      }
    }
  }
  if (matches.size() == 1) {
    return matches.front()->column.id;
  }
  std::string full_name = Text(*qualifier) + "." + Text(name);
  if (matches.empty()) {
    return Err(at, Error::kNoSuchColumn, full_name);
  }
  return Err(at, Error::kAmbiguousColumn, full_name,
             AmbiguousCandidates(matches));
}

base::StatusOr<ColumnId> Translator::ResolveColumnExpr(uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(p_, expr);
  if (node->tag != SYNTAQLITE_NODE_COLUMN_REF) {
    return Unsupported(expr, "an expression other than a column");
  }
  const SyntaqliteColumnRef& ref = node->column_ref;
  if (IsSpanPresent(ref.schema)) {
    return Unsupported(expr, "a schema-qualified column");
  }
  std::optional<NameId> table;
  if (IsSpanPresent(ref.table)) {
    table = Name(ref.table);
  }
  return Resolve(table, Name(ref.column), expr);
}

base::StatusOr<Translator::TranslatedAggregates>
Translator::TranslateAggregateList(uint32_t list_id) {
  using Function = Builder::AggregateItem::Function;
  const auto* list = Node<SyntaqlitePerfettoAggregateList>(p_, list_id);
  uint32_t count = syntaqlite_list_count(list);
  TranslatedAggregates out;
  out.items.reserve(count);
  out.nodes.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(p_, agg_id);
    const auto* node = Node<SyntaqliteNode>(p_, agg->expr);
    if (node->tag != SYNTAQLITE_NODE_FUNCTION_CALL) {
      return Expected(agg->expr, "an aggregate like SUM(column)");
    }
    const SyntaqliteFunctionCall& call = node->function_call;
    std::string function = base::ToUpper(SpanText(p_, call.func_name));
    if (call.flags.bits.distinct) {
      return Unsupported(agg->expr, "DISTINCT");
    }
    if (syntaqlite_node_is_present(call.filter_clause)) {
      return Unsupported(call.filter_clause, "FILTER");
    }
    if (syntaqlite_node_is_present(call.over_clause)) {
      return Unsupported(call.over_clause, "OVER");
    }
    Builder::AggregateItem item;
    item.name = Name(agg->name);
    if (function == "COUNT" && call.flags.bits.star) {
      item.function = Function::kCountStar;
    } else {
      if (function == "SUM") {
        item.function = Function::kSum;
      } else if (function == "MIN") {
        item.function = Function::kMin;
      } else if (function == "MAX") {
        item.function = Function::kMax;
      } else {
        return Unsupported(agg->expr, "aggregate " + function);
      }
      if (call.flags.bits.star) {
        return Expected(agg->expr, "a column name, not *");
      }
      const auto* args = Node<SyntaqliteExprList>(p_, call.args);
      if (!syntaqlite_node_is_present(call.args) ||
          syntaqlite_list_count(args) != 1) {
        return Expected(agg->expr, "exactly one argument");
      }
      uint32_t arg_id = syntaqlite_list_child_id(args, 0);
      ASSIGN_OR_RETURN(item.column, ResolveColumnExpr(arg_id));
      PlanType type = ColumnType(item.column);
      if (type.storage() &&
          !(type.Is<core::Uint32>() || type.Is<core::Int32>() ||
            type.Is<core::Int64>())) {
        return Expected(arg_id, "an integer column");
      }
    }
    out.items.push_back(item);
    out.nodes.push_back(agg_id);
  }
  return out;
}

base::Status Translator::TranslatePipeline(uint32_t pipeline) {
  const auto& n = Node<SyntaqliteNode>(p_, pipeline)->perfetto_pipeline;
  if (syntaqlite_node_is_present(n.intersection)) {
    RETURN_IF_ERROR(TranslateStage(n.intersection));
  } else if (syntaqlite_node_is_present(n.graph)) {
    RETURN_IF_ERROR(TranslateStage(n.graph));
  } else {
    RETURN_IF_ERROR(TranslateStage(n.from));
  }
  if (!syntaqlite_node_is_present(n.stages)) {
    return base::OkStatus();
  }
  const auto* stages = Node<SyntaqlitePerfettoPipeStageList>(p_, n.stages);
  uint32_t count = syntaqlite_list_count(stages);
  for (uint32_t i = 0; i < count; i++) {
    RETURN_IF_ERROR(TranslateStage(syntaqlite_list_child_id(stages, i)));
  }
  return base::OkStatus();
}

std::vector<NamedColumn> Translator::VisibleColumns() const {
  std::vector<NamedColumn> columns;
  for (const RowColumn& column : scope_.row) {
    if (!column.qualified_only) {
      columns.push_back(column.column);
    }
  }
  return columns;
}

PreparedStatement Translator::Finish() {
  std::vector<NamedColumn> visible = VisibleColumns();
  return builder_.Finish(root_, visible);
}

base::StatusOr<Builder::ScanResult> Translator::TranslateSubpipeline(
    uint32_t pipeline) {
  Scope outer = std::exchange(scope_, Scope{});
  Builder::Relation outer_root = root_;
  base::Status status = TranslatePipeline(pipeline);
  Builder::ScanResult out{root_, VisibleColumns()};
  scope_ = std::move(outer);
  root_ = outer_root;
  RETURN_IF_ERROR(status);
  return out;
}

base::StatusOr<PreparedStatement> Translate(SyntaqliteParser* parser,
                                            uint32_t pipeline,
                                            const NodeSourceFn& source,
                                            const Catalog& catalog) {
  Translator translator(parser, source, catalog);
  RETURN_IF_ERROR(translator.TranslatePipeline(pipeline));
  return translator.Finish();
}

}  // namespace perfetto::trace_processor::pipesql
