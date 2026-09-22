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
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  uint32_t len;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  PERFETTO_CHECK(text != nullptr);
  return {text, len};
}

template <typename T>
const T* Node(SyntaqliteParser* p, uint32_t id) {
  return static_cast<const T*>(syntaqlite_parser_node(p, id));
}

class Compiler {
 public:
  Compiler(SyntaqliteParser* p, const NodeSourceFn& source, const Catalog& c)
      : p_(p), source_(source), catalog_(c) {}

  base::Status CompileSource(uint32_t from);
  base::Status CompileStage(uint32_t stage);
  LogicalPlan Finish();

 private:
  // The finalized dataframe a source names, if it can be read without SQLite.
  const dataframe::Dataframe* FindDirectDataframe(
      const SyntaqlitePerfettoPipeSource&) const;
  // The name a source's columns can be qualified with, if it has one.
  std::optional<std::string> SourceQualifier(
      const SyntaqlitePerfettoPipeSource&) const;
  op::Scan CompileDataframeSource(const dataframe::Dataframe&,
                                  std::string name);
  base::StatusOr<op::Scan> CompileSqlSource(uint32_t from);
  void AddScanColumn(op::Scan&, ColumnSchema);
  base::Status CompileTreeAccumulate(uint32_t stage);
  base::StatusOr<ColumnId> CompileSum(uint32_t agg_id, uint32_t expr);
  base::StatusOr<ColumnId> Resolve(const std::string& name, uint32_t at) const {
    return Resolve("", name, at);
  }
  base::StatusOr<ColumnId> Resolve(const std::string& qualifier,
                                   const std::string& name,
                                   uint32_t at) const;

  // Every error is one of a few shapes filled in with a short noun, so a new
  // check costs one call and one short literal.
  enum class Error { kExpected, kUnsupported, kNoSuchColumn, kAmbiguousColumn };
  base::Status Err(uint32_t at,
                   Error,
                   std::string_view what,
                   const std::string& detail = "") const;
  base::Status Expected(uint32_t at, std::string_view what) const {
    return Err(at, Error::kExpected, what);
  }
  base::Status Unsupported(uint32_t at, std::string_view what) const {
    return Err(at, Error::kUnsupported, what);
  }
  // A column in scope, and the operator and AST node which put it there.
  struct Binding {
    ColumnId id;
    const char* op;
    uint32_t node;
  };
  void Bind(NamedColumn column, uint32_t node) {
    names_[base::ToLower(column.name)].push_back({column.id, op_, node});
    plan_.output.push_back(std::move(column));
  }
  std::string Origin(const Binding&, const std::string& name) const;
  std::string Traceback(uint32_t node) const {
    return source_(node).AsTraceback(0);
  }

  SyntaqliteParser* p_;
  const NodeSourceFn& source_;
  const Catalog& catalog_;
  LogicalPlan plan_;
  // The operator being compiled, which prefixes every error.
  const char* op_ = "FROM";
  base::FlatHashMap<std::string, std::vector<Binding>> names_;
  // The name columns bound by a node can be qualified with, as in
  // `name.column`.
  base::FlatHashMap<uint32_t, std::string> qualifiers_;
};

base::Status Compiler::CompileSource(uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(p_, from);
  op::Scan scan;
  if (const dataframe::Dataframe* dataframe = FindDirectDataframe(*n)) {
    scan = CompileDataframeSource(*dataframe, SpanText(p_, n->table_name));
  } else {
    ASSIGN_OR_RETURN(scan, CompileSqlSource(from));
  }
  if (std::optional<std::string> qualifier = SourceQualifier(*n)) {
    qualifiers_[from] = std::move(*qualifier);
  }
  for (const NamedColumn& column : scan.columns) {
    Bind(column, from);
  }
  plan_.ops.emplace_back(std::move(scan));
  return base::OkStatus();
}

const dataframe::Dataframe* Compiler::FindDirectDataframe(
    const SyntaqlitePerfettoPipeSource& n) const {
  // Only an unqualified table name is read directly; anything else goes to
  // SQLite. An alias only renames the qualifier, so it does not matter here.
  bool table_name =
      !syntaqlite_node_is_present(n.select) && n.schema.length == 0;
  if (!table_name) {
    return nullptr;
  }
  const dataframe::Dataframe* dataframe =
      catalog_.FindDataframe(SpanText(p_, n.table_name));
  // Columns can only be shared out of a dataframe which has stopped changing.
  return dataframe && dataframe->finalized() ? dataframe : nullptr;
}

std::optional<std::string> Compiler::SourceQualifier(
    const SyntaqlitePerfettoPipeSource& n) const {
  // As in SQLite, an alias replaces the table name.
  if (syntaqlite_node_is_present(n.alias)) {
    const auto* alias = Node<SyntaqliteName>(p_, n.alias);
    return SpanText(p_, alias->ident_name.source);
  }
  if (!syntaqlite_node_is_present(n.select)) {
    return SpanText(p_, n.table_name);
  }
  return std::nullopt;
}

op::Scan Compiler::CompileDataframeSource(const dataframe::Dataframe& dataframe,
                                          std::string name) {
  op::Scan scan;
  op::Scan::Dataframe source;
  source.name = std::move(name);
  source.row_count = dataframe.row_count();
  const std::vector<std::string>& names = dataframe.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    // TODO(lalitm): Share SQL column visibility metadata with DataframeModule
    // instead of recognizing hidden columns by name here.
    // Hidden from SELECT * in SQLite, so hidden here too.
    if (names[i] == "_auto_id") {
      continue;
    }
    AddScanColumn(scan, {names[i], dataframe.column_type(i)});
    source.columns.push_back(dataframe.shared_column(i));
  }
  scan.source = std::move(source);
  return scan;
}

base::StatusOr<op::Scan> Compiler::CompileSqlSource(uint32_t from) {
  SqlSource sql = source_(from);
  sql =
      sql.RewriteAllIgnoreExisting(SqlSource::FromTraceProcessorImplementation(
          "SELECT * FROM " + sql.sql()));
  auto described = catalog_.DescribeQuery(sql);
  if (!described.ok()) {
    return base::ErrStatus("%s%s", Traceback(from).c_str(),
                           described.status().c_message());
  }
  op::Scan scan;
  scan.source = std::move(sql);
  for (ColumnSchema& column : *described) {
    AddScanColumn(scan, std::move(column));
  }
  return scan;
}

void Compiler::AddScanColumn(op::Scan& scan, ColumnSchema column) {
  ColumnId id = plan_.AddColumn(column.name, column.type);
  scan.columns.push_back({std::move(column.name), id});
}

base::Status Compiler::CompileStage(uint32_t stage) {
  const auto* node = Node<SyntaqliteNode>(p_, stage);
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE:
      return CompileTreeAccumulate(stage);
    default:
      PERFETTO_FATAL("Unknown pipeline stage");
  }
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
  };
  const Shape& shape = kShapes[static_cast<size_t>(error)];
  return base::ErrStatus("%s%s: %s%.*s%s%s", Traceback(at).c_str(), op_,
                         shape.prefix, static_cast<int>(what.size()),
                         what.data(), shape.suffix, detail.c_str());
}

std::string Compiler::Origin(const Binding& binding,
                             const std::string& name) const {
  // A qualified name is something the user can write to pick this candidate.
  if (const std::string* qualifier = qualifiers_.Find(binding.node)) {
    return "`" + *qualifier + "." + name + "`";
  }
  constexpr size_t kMaxLen = 48;
  std::string text = source_(binding.node).sql();
  size_t len = std::min(text.find('\n'), kMaxLen);
  if (len < text.size()) {
    text = text.substr(0, len) + "...";
  }
  return "`" + std::string(binding.op) + " " + text + "`";
}

base::StatusOr<ColumnId> Compiler::Resolve(const std::string& qualifier,
                                           const std::string& name,
                                           uint32_t at) const {
  std::vector<Binding> matches;
  if (const auto* bindings = names_.Find(base::ToLower(name))) {
    for (const Binding& binding : *bindings) {
      const std::string* bound = qualifiers_.Find(binding.node);
      if (qualifier.empty() ||
          (bound && base::CaseInsensitiveEqual(*bound, qualifier))) {
        matches.push_back(binding);
      }
    }
  }
  if (matches.size() == 1) {
    return matches.front().id;
  }
  std::string full_name = qualifier.empty() ? name : qualifier + "." + name;
  if (matches.empty()) {
    return Err(at, Error::kNoSuchColumn, full_name);
  }
  std::string candidates;
  for (const Binding& binding : matches) {
    candidates += (candidates.empty() ? ": it could be " : " or ");
    candidates += Origin(binding, name);
  }
  return Err(at, Error::kAmbiguousColumn, full_name, candidates);
}

// Returns the column summed by `SUM(column)`, the only aggregate supported so
// far.
base::StatusOr<ColumnId> Compiler::CompileSum(uint32_t agg_id, uint32_t expr) {
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
  if (ref.schema.length != 0) {
    return Unsupported(arg_id, "a schema-qualified column");
  }
  std::string table = ref.table.length ? SpanText(p_, ref.table) : "";
  ASSIGN_OR_RETURN(ColumnId value,
                   Resolve(table, SpanText(p_, ref.column), agg_id));
  const auto& type = plan_.columns[value].type;
  if (type && !(type->Is<core::Id>() || type->Is<core::Uint32>() ||
                type->Is<core::Int32>() || type->Is<core::Int64>())) {
    return Expected(arg_id, "an integer column");
  }
  return value;
}

base::Status Compiler::CompileTreeAccumulate(uint32_t stage) {
  op_ = "TREE ACCUMULATE";
  const auto* n = Node<SyntaqlitePerfettoTreeAccumulate>(p_, stage);

  op::TreeAccumulate acc;
  acc.direction = n->direction == SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN
                      ? op::TreeDirection::kDown
                      : op::TreeDirection::kUp;
  ASSIGN_OR_RETURN(acc.node_column, Resolve("id", stage));
  ASSIGN_OR_RETURN(acc.parent_column, Resolve("parent_id", stage));

  std::vector<std::pair<NamedColumn, uint32_t>> output;
  const auto* list =
      Node<SyntaqlitePerfettoTreeAggregateList>(p_, n->aggregates);
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoTreeAggregate>(p_, agg_id);
    ASSIGN_OR_RETURN(ColumnId value, CompileSum(agg_id, agg->expr));
    std::string name = SpanText(p_, agg->name);
    ColumnId id = plan_.AddColumn(name, core::Int64{});
    acc.aggregates.push_back({op::TreeAccumulate::Function::kSum, value, id});
    output.push_back({NamedColumn{std::move(name), id}, agg_id});
  }
  // All expressions see the input scope. Publish this stage's bindings only
  // after resolving every aggregate; duplicate names are ambiguous on lookup.
  for (auto& [column, node] : output) {
    Bind(std::move(column), node);
  }
  plan_.ops.emplace_back(std::move(acc));
  return base::OkStatus();
}

LogicalPlan Compiler::Finish() {
  return std::move(plan_);
}

}  // namespace

base::StatusOr<LogicalPlan> Compile(SyntaqliteParser* p,
                                    uint32_t pipeline,
                                    const NodeSourceFn& source,
                                    const Catalog& catalog) {
  const auto& n = Node<SyntaqliteNode>(p, pipeline)->perfetto_pipeline;
  Compiler compiler(p, source, catalog);
  RETURN_IF_ERROR(compiler.CompileSource(n.from));
  if (syntaqlite_node_is_present(n.stages)) {
    const auto* stages = Node<SyntaqlitePerfettoPipeStageList>(p, n.stages);
    uint32_t count = syntaqlite_list_count(stages);
    for (uint32_t i = 0; i < count; i++) {
      RETURN_IF_ERROR(
          compiler.CompileStage(syntaqlite_list_child_id(stages, i)));
    }
  }
  return compiler.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
