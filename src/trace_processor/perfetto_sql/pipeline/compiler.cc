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

#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"

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
#include "src/trace_processor/perfetto_sql/pipeline/column_pruning.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"

namespace perfetto::trace_processor::pipeline {

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

base::StatusOr<ColumnId> Compiler::Resolve(NameId name, uint32_t at) const {
  return Resolve(std::nullopt, name, at);
}

Compiler::Compiler(SyntaqliteParser* p,
                   const NodeSourceFn& source,
                   const Catalog& c)
    : p_(p), source_(source), catalog_(c) {}

base::Status Compiler::Unsupported(uint32_t at, std::string_view what) const {
  return Err(at, Error::kUnsupported, what);
}

base::Status Compiler::Expected(uint32_t at, std::string_view what) const {
  return Err(at, Error::kExpected, what);
}

void Compiler::Append(NamedColumn column, uint32_t node, bool qualified_only) {
  scope_.row.push_back(
      {std::move(column), scope_.operation, node, qualified_only});
}

SqlSource Compiler::SourceForNode(uint32_t node) const {
  return source_(node);
}

void Compiler::SetOperation(const char* operation) {
  scope_.operation = operation;
}

void Compiler::ReplaceRow(std::vector<RowColumn> row) {
  scope_.row = std::move(row);
}

void Compiler::ExtendRow(std::vector<RowColumn> columns) {
  scope_.row.insert(scope_.row.end(), columns.begin(), columns.end());
}

void Compiler::DropColumn(NameId name) {
  scope_.row.erase(std::remove_if(scope_.row.begin(), scope_.row.end(),
                                  [&](const RowColumn& column) {
                                    return !column.qualified_only &&
                                           Same(column.column.name, name);
                                  }),
                   scope_.row.end());
  RemoveAlias(name);
}

void Compiler::RenameColumn(size_t at, NameId name, uint32_t node) {
  RowColumn& column = scope_.row[at];
  column.column.name = name;
  column.operation = scope_.operation;
  column.node = node;
}

void Compiler::SetColumnValue(size_t at, ColumnId value, uint32_t node) {
  RowColumn& column = scope_.row[at];
  column.column.id = value;
  column.operation = scope_.operation;
  column.node = node;
  RemoveAlias(column.column.name);
}

void Compiler::ClearAliases() {
  scope_.aliases.clear();
}

void Compiler::AddAlias(Alias alias) {
  scope_.aliases.push_back(std::move(alias));
}

void Compiler::ReplaceAliasColumns(
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

ColumnId Compiler::AddColumn(NameId name, PlanType type) {
  return plan_.AddColumn(name, type);
}

base::Status Compiler::BuildOperation(uint32_t stage) {
  uint32_t tag = Node<SyntaqliteNode>(p_, stage)->tag;
  const auto* registration = FindOperationBySyntax(tag);
  if (!registration) {
    // The grammar can accept a stage before anything lowers it.
    std::string sql = source_(stage).sql();
    std::string keyword = sql.substr(0, sql.find_first_of(" \n"));
    return source_(stage).AddTraceback(
        0, base::ErrStatus("%s is not supported yet", keyword.c_str()));
  }
  return registration->BuildPlan(this, stage);
}

base::Status Compiler::Err(uint32_t at,
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

std::string Compiler::Origin(const RowColumn& column) const {
  // A qualified name is something the user can write to pick this candidate.
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

std::string Compiler::AmbiguousCandidates(
    const std::vector<const RowColumn*>& matches) const {
  std::string candidates;
  for (const RowColumn* match : matches) {
    candidates += (candidates.empty() ? ": it could be " : " or ");
    candidates += Origin(*match);
  }
  return candidates;
}

const Compiler::Alias* Compiler::FindAlias(NameId name) const {
  for (const Alias& alias : scope_.aliases) {
    if (Same(alias.name, name)) {
      return &alias;
    }
  }
  return nullptr;
}

std::optional<ColumnId> Compiler::FindColumn(
    const std::vector<NamedColumn>& columns,
    NameId name) const {
  for (const NamedColumn& column : columns) {
    if (Same(column.name, name)) {
      return column.id;
    }
  }
  return std::nullopt;
}

void Compiler::RemoveAlias(NameId name) {
  scope_.aliases.erase(
      std::remove_if(
          scope_.aliases.begin(), scope_.aliases.end(),
          [&](const Alias& alias) { return Same(alias.name, name); }),
      scope_.aliases.end());
}

base::Status Compiler::NoSuchColumn(NameId name, uint32_t at) const {
  if (FindAlias(name)) {
    return Expected(at, "a column, but '" + Text(name) + "' is a table alias");
  }
  return Err(at, Error::kNoSuchColumn, Text(name));
}

base::StatusOr<size_t> Compiler::FindInRow(NameId name, uint32_t at) const {
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

base::Status Compiler::CheckListedOnce(std::vector<NameId>& seen,
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

base::StatusOr<ColumnId> Compiler::Resolve(std::optional<NameId> qualifier,
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

base::StatusOr<ColumnId> Compiler::ResolveColumnExpr(uint32_t expr) {
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

// Resolves an aggregate call like `SUM(column)` to one of the executor's
// shared functions and the column it reads. Its output is left to the stage.
base::StatusOr<Aggregate> Compiler::ResolveAggregate(uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(p_, expr);
  if (node->tag != SYNTAQLITE_NODE_FUNCTION_CALL) {
    return Expected(expr, "an aggregate like SUM(column)");
  }
  const SyntaqliteFunctionCall& call = node->function_call;
  std::string function = base::ToUpper(SpanText(p_, call.func_name));
  if (call.flags.bits.distinct) {
    return Unsupported(expr, "DISTINCT");
  }
  if (syntaqlite_node_is_present(call.filter_clause)) {
    return Unsupported(call.filter_clause, "FILTER");
  }
  if (syntaqlite_node_is_present(call.over_clause)) {
    return Unsupported(call.over_clause, "OVER");
  }
  Aggregate aggregate;
  if (function == "COUNT" && call.flags.bits.star) {
    aggregate.function = Aggregate::Function::kCountStar;
    return aggregate;
  }
  if (function == "SUM") {
    aggregate.function = Aggregate::Function::kSum;
  } else if (function == "MIN") {
    aggregate.function = Aggregate::Function::kMin;
  } else if (function == "MAX") {
    aggregate.function = Aggregate::Function::kMax;
  } else {
    return Unsupported(expr, "aggregate " + function);
  }
  if (call.flags.bits.star) {
    return Expected(expr, "a column name, not *");
  }
  const auto* args = Node<SyntaqliteExprList>(p_, call.args);
  if (!syntaqlite_node_is_present(call.args) ||
      syntaqlite_list_count(args) != 1) {
    return Expected(expr, "exactly one argument");
  }
  uint32_t arg_id = syntaqlite_list_child_id(args, 0);
  ASSIGN_OR_RETURN(aggregate.column, ResolveColumnExpr(arg_id));
  const auto& type = plan_.columns()[aggregate.column].type;
  if (type.storage() && !(type.Is<core::Id>() || type.Is<core::Uint32>() ||
                          type.Is<core::Int32>() || type.Is<core::Int64>())) {
    return Expected(arg_id, "an integer column");
  }
  return aggregate;
}

base::Status Compiler::CompilePipeline(uint32_t pipeline) {
  const auto& n = Node<SyntaqliteNode>(p_, pipeline)->perfetto_pipeline;
  if (syntaqlite_node_is_present(n.intersection)) {
    RETURN_IF_ERROR(BuildOperation(n.intersection));
  } else if (syntaqlite_node_is_present(n.graph)) {
    RETURN_IF_ERROR(BuildOperation(n.graph));
  } else {
    RETURN_IF_ERROR(BuildOperation(n.from));
  }
  if (!syntaqlite_node_is_present(n.stages)) {
    return base::OkStatus();
  }
  const auto* stages = Node<SyntaqlitePerfettoPipeStageList>(p_, n.stages);
  uint32_t count = syntaqlite_list_count(stages);
  for (uint32_t i = 0; i < count; i++) {
    RETURN_IF_ERROR(BuildOperation(syntaqlite_list_child_id(stages, i)));
  }
  return base::OkStatus();
}

std::vector<NamedColumn> Compiler::VisibleColumns() const {
  std::vector<NamedColumn> columns;
  for (const RowColumn& column : scope_.row) {
    if (!column.qualified_only) {
      columns.push_back(column.column);
    }
  }
  return columns;
}

LogicalPlan Compiler::Finish() {
  plan_.output() = VisibleColumns();
  return std::move(plan_);
}

base::StatusOr<std::vector<NamedColumn>> Compiler::CompileSubpipeline(
    uint32_t pipeline) {
  Scope outer = std::exchange(scope_, Scope{});
  base::Status status = CompilePipeline(pipeline);
  std::vector<NamedColumn> columns = VisibleColumns();
  scope_ = std::move(outer);
  RETURN_IF_ERROR(status);
  return columns;
}

base::StatusOr<LogicalPlan> Compile(SyntaqliteParser* p,
                                    uint32_t pipeline,
                                    const NodeSourceFn& source,
                                    const Catalog& catalog) {
  Compiler compiler(p, source, catalog);
  RETURN_IF_ERROR(compiler.CompilePipeline(pipeline));
  LogicalPlan plan = compiler.Finish();
  PruneColumns(plan);
  return plan;
}

}  // namespace perfetto::trace_processor::pipeline
