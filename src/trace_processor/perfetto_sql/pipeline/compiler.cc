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

bool IsPresent(SyntaqliteTextSpan span) {
  return span.length != 0 || syntaqlite_span_is_quoted(span);
}

base::StatusOr<ColumnId> Compiler::Resolve(const std::string& name,
                                           uint32_t at) const {
  return Resolve("", name, at);
}

Compiler::Compiler(SyntaqliteParser* p,
                   const NodeSourceFn& source,
                   const Catalog& c)
    : p_(p), source_(source), catalog_(c) {}

std::string Compiler::Traceback(uint32_t node) const {
  return source_(node).AsTraceback(0);
}

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

void Compiler::SetRow(std::vector<RowColumn> row) {
  scope_.row = std::move(row);
}

void Compiler::ExtendRow(std::vector<RowColumn> columns) {
  scope_.row.insert(scope_.row.end(), columns.begin(), columns.end());
}

void Compiler::DropColumn(const std::string& name) {
  scope_.row.erase(std::remove_if(scope_.row.begin(), scope_.row.end(),
                                  [&](const RowColumn& column) {
                                    return !column.qualified_only &&
                                           base::CaseInsensitiveEqual(
                                               column.column.name, name);
                                  }),
                   scope_.row.end());
  RemoveAlias(name);
}

void Compiler::RenameColumn(size_t at, std::string name, uint32_t node) {
  RowColumn& column = scope_.row[at];
  column.column.name = std::move(name);
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

ColumnId Compiler::AddColumn(std::string name,
                             std::optional<core::StorageType> type) {
  return plan_.AddColumn(std::move(name), type);
}

base::Status Compiler::BuildOperation(uint32_t stage) {
  const auto* definition =
      FindOperationBySyntax(Node<SyntaqliteNode>(p_, stage)->tag);
  PERFETTO_CHECK(definition);
  return definition->BuildPlan(this, stage);
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
  return base::ErrStatus(
      "%s%s: %s%.*s%s%s", Traceback(at).c_str(), scope_.operation, shape.prefix,
      static_cast<int>(what.size()), what.data(), shape.suffix, detail.c_str());
}

std::string Compiler::Origin(const RowColumn& column) const {
  // A qualified name is something the user can write to pick this candidate.
  const std::string& name = column.column.name;
  for (const Alias& alias : scope_.aliases) {
    for (const RowColumn& aliased : alias.columns) {
      if (aliased.column.id == column.column.id &&
          base::CaseInsensitiveEqual(aliased.column.name, name)) {
        return "`" + alias.name + "." + name + "`";
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

const Compiler::Alias* Compiler::FindAlias(const std::string& name) const {
  for (const Alias& alias : scope_.aliases) {
    if (base::CaseInsensitiveEqual(alias.name, name)) {
      return &alias;
    }
  }
  return nullptr;
}

void Compiler::RemoveAlias(const std::string& name) {
  scope_.aliases.erase(
      std::remove_if(scope_.aliases.begin(), scope_.aliases.end(),
                     [&](const Alias& alias) {
                       return base::CaseInsensitiveEqual(alias.name, name);
                     }),
      scope_.aliases.end());
}

base::Status Compiler::NoSuchColumn(const std::string& name,
                                    uint32_t at) const {
  if (FindAlias(name)) {
    return Expected(at, "a column, but '" + name + "' is a table alias");
  }
  return Err(at, Error::kNoSuchColumn, name);
}

base::StatusOr<size_t> Compiler::FindInRow(const std::string& name,
                                           uint32_t at) const {
  std::vector<const RowColumn*> matches;
  size_t found = 0;
  for (size_t i = 0; i < scope_.row.size(); ++i) {
    if (!scope_.row[i].qualified_only &&
        base::CaseInsensitiveEqual(scope_.row[i].column.name, name)) {
      matches.push_back(&scope_.row[i]);
      found = i;
    }
  }
  if (matches.empty()) {
    return NoSuchColumn(name, at);
  }
  if (matches.size() > 1) {
    return Err(at, Error::kAmbiguousColumn, name, AmbiguousCandidates(matches));
  }
  return found;
}

base::Status Compiler::CheckListedOnce(std::vector<std::string>& seen,
                                       const std::string& name,
                                       uint32_t at) const {
  for (const std::string& other : seen) {
    if (base::CaseInsensitiveEqual(other, name)) {
      return Expected(at, "'" + name + "' to be listed only once");
    }
  }
  seen.push_back(name);
  return base::OkStatus();
}

base::StatusOr<ColumnId> Compiler::Resolve(const std::string& qualifier,
                                           const std::string& name,
                                           uint32_t at) const {
  if (qualifier.empty()) {
    ASSIGN_OR_RETURN(size_t i, FindInRow(name, at));
    return scope_.row[i].column.id;
  }
  std::string full_name = qualifier + "." + name;
  const Alias* alias = FindAlias(qualifier);
  if (!alias) {
    return Err(at, Error::kNoSuchColumn, full_name);
  }
  std::vector<const RowColumn*> matches;
  for (const RowColumn& column : alias->columns) {
    if (base::CaseInsensitiveEqual(column.column.name, name)) {
      matches.push_back(&column);
    }
  }
  if (matches.empty()) {
    return Err(at, Error::kNoSuchColumn, full_name);
  }
  if (matches.size() > 1) {
    return Err(at, Error::kAmbiguousColumn, full_name,
               AmbiguousCandidates(matches));
  }
  return matches.front()->column.id;
}

// Returns the column summed by `SUM(column)`, the only aggregate supported so
// far.
base::StatusOr<ColumnId> Compiler::ResolveSum(uint32_t agg_id, uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(p_, expr);
  if (node->tag != SYNTAQLITE_NODE_FUNCTION_CALL) {
    return Expected(expr, "an aggregate like SUM(column)");
  }
  const SyntaqliteFunctionCall& call = node->function_call;
  std::string function = base::ToUpper(SpanText(p_, call.func_name));
  if (function != "SUM") {
    return Unsupported(expr, "aggregate " + function);
  }
  if (call.flags.bits.star) {
    return Expected(expr, "a column name, not *");
  }
  if (call.flags.bits.distinct) {
    return Unsupported(expr, "DISTINCT");
  }
  if (syntaqlite_node_is_present(call.filter_clause)) {
    return Unsupported(call.filter_clause, "FILTER");
  }
  if (syntaqlite_node_is_present(call.over_clause)) {
    return Unsupported(call.over_clause, "OVER");
  }
  const auto* args = Node<SyntaqliteExprList>(p_, call.args);
  if (!syntaqlite_node_is_present(call.args) ||
      syntaqlite_list_count(args) != 1) {
    return Expected(expr, "exactly one argument");
  }
  uint32_t arg_id = syntaqlite_list_child_id(args, 0);
  const auto* arg = Node<SyntaqliteNode>(p_, arg_id);
  if (arg->tag != SYNTAQLITE_NODE_COLUMN_REF) {
    return Expected(arg_id, "a column name");
  }
  const SyntaqliteColumnRef& ref = arg->column_ref;
  if (IsPresent(ref.schema)) {
    return Unsupported(arg_id, "a schema-qualified column");
  }
  std::string table = IsPresent(ref.table) ? SpanText(p_, ref.table) : "";
  ASSIGN_OR_RETURN(ColumnId value,
                   Resolve(table, SpanText(p_, ref.column), agg_id));
  const auto& type = plan_.columns()[value].type;
  if (type && !(type->Is<core::Id>() || type->Is<core::Uint32>() ||
                type->Is<core::Int32>() || type->Is<core::Int64>())) {
    return Expected(arg_id, "an integer column");
  }
  return value;
}

bool Compiler::IsCountStar(uint32_t expr) const {
  const auto* node = Node<SyntaqliteNode>(p_, expr);
  if (node->tag != SYNTAQLITE_NODE_FUNCTION_CALL) {
    return false;
  }
  const SyntaqliteFunctionCall& call = node->function_call;
  return call.flags.bits.star && !call.flags.bits.distinct &&
         !syntaqlite_node_is_present(call.filter_clause) &&
         !syntaqlite_node_is_present(call.over_clause) &&
         base::CaseInsensitiveEqual(SpanText(p_, call.func_name), "COUNT");
}

base::Status Compiler::CompilePipeline(uint32_t pipeline) {
  const auto& n = Node<SyntaqliteNode>(p_, pipeline)->perfetto_pipeline;
  if (syntaqlite_node_is_present(n.intersection)) {
    RETURN_IF_ERROR(BuildOperation(n.intersection));
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

LogicalPlan Compiler::Finish() {
  plan_.output().clear();
  for (const RowColumn& column : scope_.row) {
    plan_.output().push_back(column.column);
  }
  return std::move(plan_);
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
