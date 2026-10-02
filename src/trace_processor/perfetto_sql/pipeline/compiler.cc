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
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/column_pruning.h"
#include "src/trace_processor/perfetto_sql/pipeline/filter_pushdown.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor::pipeline {
namespace {

namespace analysis = ::perfetto::perfetto_sql::analysis;
using core::StorageType;

// The name `span` spells. A quoted name escapes its closing quote by doubling
// it, which the span, pointing into the source, still contains.
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

// Whether `span` was written at all. An empty quoted name, like `""`, has
// no length but is still there.
bool IsPresent(SyntaqliteTextSpan span) {
  return span.length != 0 || syntaqlite_span_is_quoted(span);
}

template <typename T>
const T* Node(SyntaqliteParser* p, uint32_t id) {
  return static_cast<const T*>(syntaqlite_parser_node(p, id));
}

class Compiler {
 public:
  Compiler(SyntaqliteParser* p, const NodeSourceFn& source, const Catalog& c)
      : p_(p), source_(source), catalog_(c) {}

  // Compiles the pipeline at `pipeline`: its source and each of its stages.
  base::Status CompilePipeline(uint32_t pipeline);
  LogicalPlan Finish();

 private:
  // A column of the row a stage produces, and where it came from.
  struct RowColumn {
    NamedColumn column;
    // The operator and AST node which put it here, for error messages.
    const char* op;
    uint32_t node;
    // Not found by a bare name. An intersection's operand columns are like
    // this, so a bare `ts` or `dur` always means the region being carried.
    bool qualified_only = false;
  };
  // A table alias: the row's columns as they were when the alias was given.
  // Dropping, renaming or replacing a column later does not change them.
  struct Alias {
    std::string name;
    std::vector<RowColumn> columns;
  };
  // What names mean while a pipeline is compiled.
  struct Scope {
    // The operator being compiled, which prefixes every error.
    const char* op = "FROM";
    // The row as of the stage being compiled, and the table aliases in scope.
    std::vector<RowColumn> row;
    std::vector<Alias> aliases;
  };
  // A relation a source reads: the plan node producing it and its columns.
  struct Relation {
    PlanNodeId node = 0;
    std::vector<NamedColumn> columns;
  };

  base::Status CompileSource(uint32_t from);
  base::Status CompileIntersection(uint32_t node);
  base::Status CompileStage(uint32_t stage);
  base::Status CompileWhere(uint32_t stage);
  // Adds the conditions `expr` joins with AND to `conditions`.
  base::Status CompileConditions(uint32_t expr,
                                 std::vector<op::FilterCondition>& conditions);
  base::StatusOr<op::FilterCondition> CompileCondition(uint32_t expr);
  base::StatusOr<ColumnId> CompileConditionColumn(uint32_t ref) const;
  bool IsColumn(uint32_t expr) const {
    return Node<SyntaqliteNode>(p_, expr)->tag == SYNTAQLITE_NODE_COLUMN_REF;
  }
  base::StatusOr<op::FilterValue> CompileValue(uint32_t value) const;
  // Refuses a condition which is not yet one a filter can run.
  base::Status UnsupportedCondition(uint32_t at, std::string_view what) const;
  base::Status CompileSelect(uint32_t stage);
  base::Status CompileExtend(uint32_t stage);
  base::Status CompileDrop(uint32_t stage);
  base::Status CompileRename(uint32_t stage);
  base::Status CompileSet(uint32_t stage);
  base::Status CompileAs(uint32_t stage);
  // The columns a SELECT or EXTEND list produces, resolved against the row
  // before the stage.
  base::StatusOr<std::vector<RowColumn>> CompileItems(uint32_t list);
  base::StatusOr<std::vector<RowColumn>> ExpandStar(uint32_t star);

  // Appends a column to the row.
  void Append(NamedColumn column, uint32_t node, bool qualified_only = false) {
    scope_.row.push_back({std::move(column), scope_.op, node, qualified_only});
  }
  const Alias* FindAlias(const std::string& name) const;
  // Forgets the table alias `name`, if there is one.
  void RemoveAlias(const std::string& name);
  // The position in the row of the one column a bare `name` finds.
  base::StatusOr<size_t> FindInRow(const std::string& name, uint32_t at) const;
  // The error for a bare `name` which finds no column.
  base::Status NoSuchColumn(const std::string& name, uint32_t at) const;
  // Fails if a name appears twice in a list of names.
  base::Status CheckListedOnce(std::vector<std::string>& seen,
                               const std::string& name,
                               uint32_t at) const;
  // The finalized dataframe a source names, if it can be read without SQLite.
  const dataframe::Dataframe* FindDirectDataframe(
      const SyntaqlitePerfettoPipeSource&) const;
  // The name a source's columns can be qualified with, if it has one.
  std::optional<std::string> SourceQualifier(
      const SyntaqlitePerfettoPipeSource&) const;
  // Compiles what a source reads: a dataframe or SQL.
  base::StatusOr<Relation> CompileRelation(uint32_t source);
  // Fails unless every column of a source has a name a pipeline can use.
  // Crossing from SQL into a pipeline needs proper names, as creating a
  // PERFETTO TABLE does.
  base::Status CheckSourceNames(const std::vector<NamedColumn>&,
                                uint32_t at) const;
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
  enum class Error {
    kExpected,
    kUnsupported,
    kNoSuchColumn,
    kAmbiguousColumn,
    kNoSuchAlias,
  };
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
  // How a user could write a reference to `column`, to tell candidates apart.
  std::string Origin(const RowColumn& column) const;
  std::string AmbiguousCandidates(
      const std::vector<const RowColumn*>& matches) const;
  std::string Traceback(uint32_t node) const {
    return source_(node).AsTraceback(0);
  }

  SyntaqliteParser* p_;
  const NodeSourceFn& source_;
  const Catalog& catalog_;
  LogicalPlan plan_;
  Scope scope_;
};

// The column of a relation named `name`, or nothing when it has none.
std::optional<ColumnId> FindColumn(const std::vector<NamedColumn>& columns,
                                   const std::string& name) {
  for (const NamedColumn& column : columns) {
    if (base::CaseInsensitiveEqual(column.name, name)) {
      return column.id;
    }
  }
  return std::nullopt;
}

base::Status Compiler::CompileIntersection(uint32_t node) {
  scope_.op = "INTERVAL INTERSECTION";
  const auto* n = Node<SyntaqlitePerfettoIntervalIntersection>(p_, node);
  const auto* list = Node<SyntaqlitePerfettoPipeSourceList>(p_, n->operands);
  uint32_t count = syntaqlite_list_count(list);
  if (count < 2) {
    return Expected(node, "at least two relations to intersect");
  }
  const SyntaqlitePerfettoPerColumnList* per =
      syntaqlite_node_is_present(n->per)
          ? Node<SyntaqlitePerfettoPerColumnList>(p_, n->per)
          : nullptr;
  uint32_t key_count = per ? syntaqlite_list_count(per) : 0;

  op::IntervalIntersect isect;
  // The region's bounds are the operator's own, so they are named before an
  // operand can bind anything.
  isect.ts = plan_.AddColumn("ts", core::Int64{});
  isect.dur = plan_.AddColumn("dur", core::Int64{});

  Append({"ts", isect.ts}, node);
  Append({"dur", isect.dur}, node);
  std::vector<PlanNodeId> children;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t source_id = syntaqlite_list_child_id(list, i);
    const auto* source = Node<SyntaqlitePerfettoPipeSource>(p_, source_id);
    std::optional<std::string> alias = SourceQualifier(*source);
    if (!alias) {
      return Expected(source_id,
                      "an alias for the relation, as in `(...) AS x`");
    }
    if (FindAlias(*alias)) {
      return Expected(source_id, "a different alias for each relation");
    }
    ASSIGN_OR_RETURN(Relation relation, CompileRelation(source_id));
    std::optional<ColumnId> ts = FindColumn(relation.columns, "ts");
    std::optional<ColumnId> dur = FindColumn(relation.columns, "dur");
    if (!ts || !dur) {
      return Expected(source_id,
                      *alias + " to have a " + (ts ? "dur" : "ts") + " column");
    }
    op::IntervalIntersect::Operand operand;
    operand.ts = *ts;
    operand.dur = *dur;
    for (uint32_t k = 0; k < key_count; k++) {
      uint32_t col_id = syntaqlite_list_child_id(per, k);
      const auto* col = Node<SyntaqlitePerfettoPerColumn>(p_, col_id);
      std::string name = SpanText(p_, col->name);
      std::optional<ColumnId> key = FindColumn(relation.columns, name);
      if (!key) {
        return Expected(col_id, *alias + " to have a " + name + " column");
      }
      operand.keys.push_back(*key);
    }
    // Every operand column is carried, reachable through the operand's alias.
    // A PER column holds the same value in every operand, so the first
    // operand's is also the one a bare name finds.
    Alias operand_alias{*alias, {}};
    for (const NamedColumn& column : relation.columns) {
      bool is_key = std::find(operand.keys.begin(), operand.keys.end(),
                              column.id) != operand.keys.end();
      Append(column, source_id, /*qualified_only=*/!(is_key && i == 0));
      operand_alias.columns.push_back(scope_.row.back());
      operand.carried.push_back(column.id);
    }
    scope_.aliases.push_back(std::move(operand_alias));
    children.push_back(relation.node);
    isect.operands.push_back(std::move(operand));
  }
  plan_.AddNode(std::move(isect), std::move(children));
  return base::OkStatus();
}

base::Status Compiler::CompileSource(uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(p_, from);
  ASSIGN_OR_RETURN(Relation relation, CompileRelation(from));
  for (NamedColumn& column : relation.columns) {
    Append(std::move(column), from);
  }
  if (std::optional<std::string> qualifier = SourceQualifier(*n)) {
    scope_.aliases.push_back({std::move(*qualifier), scope_.row});
  }
  return base::OkStatus();
}

base::StatusOr<Compiler::Relation> Compiler::CompileRelation(uint32_t source) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(p_, source);
  op::Scan scan;
  if (const dataframe::Dataframe* dataframe = FindDirectDataframe(*n)) {
    scan = CompileDataframeSource(*dataframe, SpanText(p_, n->table_name));
  } else {
    ASSIGN_OR_RETURN(scan, CompileSqlSource(source));
  }
  RETURN_IF_ERROR(CheckSourceNames(scan.columns, source));
  Relation relation;
  relation.columns = scan.columns;
  relation.node = plan_.AddNode(std::move(scan));
  return relation;
}

const dataframe::Dataframe* Compiler::FindDirectDataframe(
    const SyntaqlitePerfettoPipeSource& n) const {
  // Only an unqualified table name is read directly; anything else goes to
  // SQLite. An alias only renames the qualifier, so it does not matter here.
  bool table_name =
      !syntaqlite_node_is_present(n.select) && !IsPresent(n.schema);
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

base::Status Compiler::CheckSourceNames(const std::vector<NamedColumn>& columns,
                                        uint32_t at) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (base::CaseInsensitiveEqual(columns[j].name, columns[i].name)) {
        return Expected(at, "distinct column names, but there are two named '" +
                                columns[j].name + "'");
      }
    }
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string& name = columns[i].name;
    // An expression is only named by its alias.
    if (name.empty()) {
      return Expected(at, "every column to have a name, but column " +
                              std::to_string(i + 1) +
                              " has none: give it one with AS");
    }
    if (!sql_argument::IsValidColumnName(base::StringView(name))) {
      return Expected(at, "every column to have a valid name, but '" + name +
                              "' is not one: give it one with AS");
    }
  }
  return base::OkStatus();
}

op::Scan Compiler::CompileDataframeSource(const dataframe::Dataframe& dataframe,
                                          std::string name) {
  op::Scan scan;
  op::Scan::Dataframe source;
  source.name = std::move(name);
  source.row_count = dataframe.row_count();
  const std::vector<std::string>& names = dataframe.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    // Hidden from SELECT * in SQLite, so hidden here too.
    if (dataframe::IsHiddenColumn(names[i])) {
      continue;
    }
    AddScanColumn(scan, {names[i], dataframe.column_type(i)});
    source.columns.push_back(dataframe.shared_column(i));
  }
  scan.source = std::move(source);
  return scan;
}

base::StatusOr<op::Scan> Compiler::CompileSqlSource(uint32_t from) {
  // The columns come from semantic analysis alone: the SQL is only run where
  // the pipeline is written, which is the only place everything it reads is
  // in scope.
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(p_, from);
  if (IsPresent(n->schema)) {
    return Err(from, Error::kUnsupported,
               "reading a relation whose columns cannot be worked out",
               " (a schema-qualified table)");
  }
  analysis::RelationAnalyzer analyzer(catalog_);
  base::StatusOr<analysis::RelationLineage> lineage =
      syntaqlite_node_is_present(n->select)
          ? analyzer.AnalyzeQuery({p_, n->select})
          : analyzer.AnalyzeRelation({p_, from}, SpanText(p_, n->table_name));
  if (!lineage.ok()) {
    return Err(from, Error::kUnsupported,
               "reading a relation whose columns cannot be worked out",
               " (" + lineage.status().message() + ")");
  }
  op::Scan scan;
  // The relation as written, which the SQL building its dataframe reads.
  scan.source = source_(from);
  for (const analysis::ColumnLineage& column : lineage->columns()) {
    std::optional<StorageType> type;
    if (std::optional<analysis::ColumnType> traced = column.type()) {
      type = sql_schema::ToStorageType(*traced);
      // An Id only means something in the table it indexes: read through
      // SQL, it is a plain number.
      if (type->Is<core::Id>()) {
        type = StorageType{core::Uint32{}};
      }
    }
    AddScanColumn(scan, {std::string(column.output_name), type});
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
    case SYNTAQLITE_NODE_PERFETTO_PIPE_WHERE:
      return CompileWhere(stage);
    case SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE:
      return CompileTreeAccumulate(stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_SELECT:
      return CompileSelect(stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_EXTEND:
      return CompileExtend(stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_DROP:
      return CompileDrop(stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_RENAME:
      return CompileRename(stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_SET:
      return CompileSet(stage);
    case SYNTAQLITE_NODE_PERFETTO_PIPE_AS:
      return CompileAs(stage);
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
      {"no such table alias: '", "'"},
  };
  const Shape& shape = kShapes[static_cast<size_t>(error)];
  return base::ErrStatus("%s%s: %s%.*s%s%s", Traceback(at).c_str(), scope_.op,
                         shape.prefix, static_cast<int>(what.size()),
                         what.data(), shape.suffix, detail.c_str());
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
  return "`" + std::string(column.op) + " " + text + "`";
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
  if (IsPresent(ref.schema)) {
    return Unsupported(arg_id, "a schema-qualified column");
  }
  std::string table = IsPresent(ref.table) ? SpanText(p_, ref.table) : "";
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
  scope_.op = "TREE ACCUMULATE";
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
  // All expressions see the input scope. Add this stage's columns only after
  // resolving every aggregate; duplicate names are ambiguous on lookup.
  for (auto& [column, node] : output) {
    Append(std::move(column), node);
  }
  plan_.AddNode(std::move(acc), {plan_.root});
  return base::OkStatus();
}

base::Status Compiler::CompileWhere(uint32_t stage) {
  scope_.op = "WHERE";
  const auto* n = Node<SyntaqlitePerfettoPipeWhere>(p_, stage);
  op::Filter filter;
  RETURN_IF_ERROR(CompileConditions(n->condition, filter.conditions));
  // The rows change but the row does not: every name still means the same.
  plan_.AddNode(std::move(filter), {plan_.root});
  return base::OkStatus();
}

base::Status Compiler::UnsupportedCondition(uint32_t at,
                                            std::string_view what) const {
  return Err(at, Error::kUnsupported, what,
             ": a condition compares a column with values, as in `dur > 0`, "
             "`name IS NULL` or `cpu IN (1, 2)`, joined by AND");
}

base::Status Compiler::CompileConditions(
    uint32_t expr,
    std::vector<op::FilterCondition>& conditions) {
  const auto* node = Node<SyntaqliteNode>(p_, expr);
  if (node->tag == SYNTAQLITE_NODE_PAREN_EXPR) {
    return CompileConditions(node->paren_expr.expr, conditions);
  }
  if (node->tag == SYNTAQLITE_NODE_BINARY_EXPR &&
      node->binary_expr.op == SYNTAQLITE_BINARY_OP_AND) {
    RETURN_IF_ERROR(CompileConditions(node->binary_expr.left, conditions));
    return CompileConditions(node->binary_expr.right, conditions);
  }
  ASSIGN_OR_RETURN(op::FilterCondition condition, CompileCondition(expr));
  conditions.push_back(std::move(condition));
  return base::OkStatus();
}

// One condition: `column op value` either way round, `column IS [NOT] NULL`
// or `column IN (value, ...)`.
base::StatusOr<op::FilterCondition> Compiler::CompileCondition(uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(p_, expr);
  op::FilterCondition condition;
  uint32_t column;
  std::vector<uint32_t> values;
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_BINARY_EXPR: {
      const SyntaqliteBinaryExpr& e = node->binary_expr;
      // Written the other way round, the comparison flips.
      bool flip = !IsColumn(e.left);
      column = flip ? e.right : e.left;
      values.push_back(flip ? e.left : e.right);
      switch (static_cast<int>(e.op)) {
        case SYNTAQLITE_BINARY_OP_EQ:
        case SYNTAQLITE_BINARY_OP_EQ_DOUBLE:
          condition.op = core::Eq{};
          break;
        case SYNTAQLITE_BINARY_OP_NE:
        case SYNTAQLITE_BINARY_OP_NE_ANGLE:
          condition.op = core::Ne{};
          break;
        case SYNTAQLITE_BINARY_OP_LT:
          condition.op = flip ? core::Op(core::Gt{}) : core::Op(core::Lt{});
          break;
        case SYNTAQLITE_BINARY_OP_LE:
          condition.op = flip ? core::Op(core::Ge{}) : core::Op(core::Le{});
          break;
        case SYNTAQLITE_BINARY_OP_GT:
          condition.op = flip ? core::Op(core::Lt{}) : core::Op(core::Gt{});
          break;
        case SYNTAQLITE_BINARY_OP_GE:
          condition.op = flip ? core::Op(core::Le{}) : core::Op(core::Ge{});
          break;
        default:
          return UnsupportedCondition(expr, "this operator");
      }
      break;
    }
    case SYNTAQLITE_NODE_IS_EXPR: {
      const SyntaqliteIsExpr& e = node->is_expr;
      column = e.left;
      switch (static_cast<int>(e.op)) {
        case SYNTAQLITE_IS_OP_IS_NULL:
          condition.op = core::IsNull{};
          break;
        case SYNTAQLITE_IS_OP_NOT_NULL:
        case SYNTAQLITE_IS_OP_NOT_NULL_SPACED:
          condition.op = core::IsNotNull{};
          break;
        case SYNTAQLITE_IS_OP_IS:
        case SYNTAQLITE_IS_OP_IS_NOT: {
          const auto* right = Node<SyntaqliteNode>(p_, e.right);
          if (right->tag != SYNTAQLITE_NODE_LITERAL ||
              right->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_NULL) {
            return UnsupportedCondition(expr, "IS with anything but NULL");
          }
          condition.op = e.op == SYNTAQLITE_IS_OP_IS
                             ? core::Op(core::IsNull{})
                             : core::Op(core::IsNotNull{});
          break;
        }
        default:
          return UnsupportedCondition(expr, "this operator");
      }
      break;
    }
    case SYNTAQLITE_NODE_IN_EXPR: {
      const SyntaqliteInExpr& e = node->in_expr;
      if (e.negated) {
        return UnsupportedCondition(expr, "NOT IN");
      }
      column = e.operand;
      condition.op = core::In{};
      if (syntaqlite_node_is_present(e.source)) {
        const auto* source = Node<SyntaqliteNode>(p_, e.source);
        if (source->tag != SYNTAQLITE_NODE_EXPR_LIST) {
          return UnsupportedCondition(e.source, "IN with anything but values");
        }
        const auto* list = Node<SyntaqliteExprList>(p_, e.source);
        for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
          values.push_back(syntaqlite_list_child_id(list, i));
        }
      }
      break;
    }
    default:
      return UnsupportedCondition(expr, "this condition");
  }
  if (!IsColumn(column)) {
    return UnsupportedCondition(column, "comparing anything but a column");
  }
  ASSIGN_OR_RETURN(condition.column, CompileConditionColumn(column));
  for (uint32_t value_id : values) {
    ASSIGN_OR_RETURN(op::FilterValue value, CompileValue(value_id));
    condition.values.push_back(std::move(value));
  }
  return condition;
}

base::StatusOr<ColumnId> Compiler::CompileConditionColumn(
    uint32_t ref_id) const {
  const SyntaqliteColumnRef& ref = Node<SyntaqliteNode>(p_, ref_id)->column_ref;
  if (IsPresent(ref.schema)) {
    return Unsupported(ref_id, "a schema-qualified column");
  }
  std::string qualifier = IsPresent(ref.table) ? SpanText(p_, ref.table) : "";
  return Resolve(qualifier, SpanText(p_, ref.column), ref_id);
}

// A value written in the pipeline: a literal, perhaps negated.
base::StatusOr<op::FilterValue> Compiler::CompileValue(uint32_t value) const {
  const auto* node = Node<SyntaqliteNode>(p_, value);
  bool negate = node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
                node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS;
  if (negate) {
    node = Node<SyntaqliteNode>(p_, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL) {
    return UnsupportedCondition(value, "comparing with anything but a value");
  }
  // The literal as written, including a string's quotes.
  std::string text = SpanText(p_, node->literal.source);
  switch (static_cast<int>(node->literal.literal_type)) {
    case SYNTAQLITE_LITERAL_TYPE_INTEGER:
      if (std::optional<int64_t> i = base::StringToInt64(text)) {
        return op::FilterValue(negate ? -*i : *i);
      }
      return Expected(value, "an integer which fits in 64 bits");
    case SYNTAQLITE_LITERAL_TYPE_FLOAT:
      if (std::optional<double> d = base::StringToDouble(text)) {
        return op::FilterValue(negate ? -*d : *d);
      }
      return Expected(value, "a number");
    case SYNTAQLITE_LITERAL_TYPE_STRING:
      if (negate) {
        return Expected(value, "a number after the minus");
      }
      return op::FilterValue(
          base::ReplaceAll(text.substr(1, text.size() - 2), "''", "'"));
    case SYNTAQLITE_LITERAL_TYPE_NULL:
      return Expected(value, "a value: compare with NULL using IS [NOT] NULL");
    default:
      return UnsupportedCondition(value, "this value");
  }
}

// The stages below are relational operators which only change which columns
// the row has and what they are called: nothing runs. Where an operator could
// take an expression, it takes a column reference for now.

base::StatusOr<std::vector<Compiler::RowColumn>> Compiler::ExpandStar(
    uint32_t star_id) {
  const auto* star = Node<SyntaqlitePerfettoPipeStar>(p_, star_id);
  std::vector<RowColumn> columns;
  if (IsPresent(star->qualifier)) {
    std::string qualifier = SpanText(p_, star->qualifier);
    const Alias* alias = FindAlias(qualifier);
    if (!alias) {
      return Err(star_id, Error::kNoSuchAlias, qualifier);
    }
    columns = alias->columns;
  } else {
    // EXTEND only takes `alias.*`: a bare star would repeat the whole row.
    if (std::string_view(scope_.op) == "EXTEND") {
      return Expected(star_id, "a table alias before the star, as in `t.*`");
    }
    columns = scope_.row;
  }

  if (syntaqlite_node_is_present(star->except)) {
    const auto* list = Node<SyntaqlitePerfettoPipeNameList>(p_, star->except);
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      std::string name =
          SpanText(p_, Node<SyntaqlitePerfettoPipeName>(p_, item_id)->name);
      RETURN_IF_ERROR(CheckListedOnce(seen, name, item_id));
      // Every column of that name goes.
      auto matches = [&](const RowColumn& column) {
        return base::CaseInsensitiveEqual(column.column.name, name);
      };
      auto it = std::remove_if(columns.begin(), columns.end(), matches);
      if (it == columns.end()) {
        return Err(item_id, Error::kNoSuchColumn, name);
      }
      columns.erase(it, columns.end());
    }
    if (columns.empty()) {
      return Expected(star_id, "a column to be left after EXCEPT");
    }
  }

  if (syntaqlite_node_is_present(star->replace)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeColumnList>(p_, star->replace);
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      const auto* item = Node<SyntaqlitePerfettoPipeColumn>(p_, item_id);
      std::string target = SpanText(p_, item->alias);
      RETURN_IF_ERROR(CheckListedOnce(seen, target, item_id));
      std::vector<RowColumn*> matches;
      for (RowColumn& column : columns) {
        if (base::CaseInsensitiveEqual(column.column.name, target)) {
          matches.push_back(&column);
        }
      }
      if (matches.empty()) {
        return Err(item_id, Error::kNoSuchColumn, target);
      }
      if (matches.size() > 1) {
        return Err(item_id, Error::kAmbiguousColumn, target,
                   AmbiguousCandidates({matches.begin(), matches.end()}));
      }
      std::string qualifier =
          IsPresent(item->qualifier) ? SpanText(p_, item->qualifier) : "";
      ASSIGN_OR_RETURN(ColumnId value,
                       Resolve(qualifier, SpanText(p_, item->name), item_id));
      *matches.front() = {{std::move(target), value}, scope_.op, item_id};
    }
  }

  // Whatever a star lists is a column of the new row like any other.
  for (RowColumn& column : columns) {
    column.qualified_only = false;
  }
  return columns;
}

base::StatusOr<std::vector<Compiler::RowColumn>> Compiler::CompileItems(
    uint32_t list_id) {
  const auto* list = Node<SyntaqlitePerfettoPipeSelectItemList>(p_, list_id);
  std::vector<RowColumn> columns;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    if (Node<SyntaqliteNode>(p_, item_id)->tag ==
        SYNTAQLITE_NODE_PERFETTO_PIPE_STAR) {
      ASSIGN_OR_RETURN(std::vector<RowColumn> expanded, ExpandStar(item_id));
      columns.insert(columns.end(), expanded.begin(), expanded.end());
      continue;
    }
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(p_, item_id);
    std::string qualifier =
        IsPresent(item->qualifier) ? SpanText(p_, item->qualifier) : "";
    std::string name = SpanText(p_, item->name);
    ASSIGN_OR_RETURN(ColumnId id, Resolve(qualifier, name, item_id));
    if (IsPresent(item->alias)) {
      name = SpanText(p_, item->alias);
    }
    columns.push_back({{std::move(name), id}, scope_.op, item_id});
  }
  return columns;
}

// Replaces the row. What it leaves is a new table: no alias reaches into it.
base::Status Compiler::CompileSelect(uint32_t stage) {
  scope_.op = "SELECT";
  const auto* n = Node<SyntaqlitePerfettoPipeSelect>(p_, stage);
  ASSIGN_OR_RETURN(scope_.row, CompileItems(n->columns));
  scope_.aliases.clear();
  return base::OkStatus();
}

// Adds columns to the row. Items see only the row before the stage, not each
// other.
base::Status Compiler::CompileExtend(uint32_t stage) {
  scope_.op = "EXTEND";
  const auto* n = Node<SyntaqlitePerfettoPipeExtend>(p_, stage);
  ASSIGN_OR_RETURN(std::vector<RowColumn> columns, CompileItems(n->columns));
  scope_.row.insert(scope_.row.end(), columns.begin(), columns.end());
  return base::OkStatus();
}

// Removes every column of each name. Aliases still reach the dropped columns,
// except an alias of the same name, which the name now hides.
base::Status Compiler::CompileDrop(uint32_t stage) {
  scope_.op = "DROP";
  const auto* n = Node<SyntaqlitePerfettoPipeDrop>(p_, stage);
  const auto* list = Node<SyntaqlitePerfettoPipeNameList>(p_, n->columns);
  std::vector<std::string> names;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    std::string name =
        SpanText(p_, Node<SyntaqlitePerfettoPipeName>(p_, item_id)->name);
    RETURN_IF_ERROR(CheckListedOnce(names, name, item_id));
    bool found =
        std::any_of(scope_.row.begin(), scope_.row.end(), [&](const auto& c) {
          return !c.qualified_only &&
                 base::CaseInsensitiveEqual(c.column.name, name);
        });
    if (!found) {
      return NoSuchColumn(name, item_id);
    }
  }
  for (const std::string& name : names) {
    scope_.row.erase(std::remove_if(scope_.row.begin(), scope_.row.end(),
                                    [&](const RowColumn& c) {
                                      return !c.qualified_only &&
                                             base::CaseInsensitiveEqual(
                                                 c.column.name, name);
                                    }),
                     scope_.row.end());
    RemoveAlias(name);
  }
  if (scope_.row.empty()) {
    return Expected(stage, "a column to be left after DROP");
  }
  return base::OkStatus();
}

// Renames columns in place. Each name must find exactly one column, and all
// renames happen at once, so two columns can swap names. Aliases still reach
// the columns under their old names.
base::Status Compiler::CompileRename(uint32_t stage) {
  scope_.op = "RENAME";
  const auto* n = Node<SyntaqlitePerfettoPipeRename>(p_, stage);
  const auto* list = Node<SyntaqlitePerfettoPipeColumnList>(p_, n->columns);
  std::vector<std::string> seen;
  std::vector<std::pair<size_t, uint32_t>> renames;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(p_, item_id);
    std::string name = SpanText(p_, item->name);
    RETURN_IF_ERROR(CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, FindInRow(name, item_id));
    renames.push_back({at, item_id});
  }
  for (const auto& [at, item_id] : renames) {
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(p_, item_id);
    scope_.row[at].column.name = SpanText(p_, item->alias);
    scope_.row[at].op = scope_.op;
    scope_.row[at].node = item_id;
  }
  return base::OkStatus();
}

// Replaces the values of columns in place. Each name must find exactly one
// column, and every value is read from the row before the stage. Aliases
// still reach the old values, except an alias of the same name, which the
// name now hides.
base::Status Compiler::CompileSet(uint32_t stage) {
  scope_.op = "SET";
  const auto* n = Node<SyntaqlitePerfettoPipeSet>(p_, stage);
  const auto* list = Node<SyntaqlitePerfettoPipeSetItemList>(p_, n->items);
  std::vector<std::string> seen;
  struct Assignment {
    size_t at;
    ColumnId value;
    uint32_t node;
  };
  std::vector<Assignment> assignments;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeSetItem>(p_, item_id);
    std::string name = SpanText(p_, item->name);
    RETURN_IF_ERROR(CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, FindInRow(name, item_id));
    const auto* value = Node<SyntaqlitePerfettoPipeColumn>(p_, item->value);
    std::string qualifier =
        IsPresent(value->qualifier) ? SpanText(p_, value->qualifier) : "";
    ASSIGN_OR_RETURN(ColumnId id, Resolve(qualifier, SpanText(p_, value->name),
                                          item->value));
    assignments.push_back({at, id, item_id});
  }
  for (const Assignment& assignment : assignments) {
    RowColumn& column = scope_.row[assignment.at];
    column.column.id = assignment.value;
    column.op = scope_.op;
    column.node = assignment.node;
    RemoveAlias(column.column.name);
  }
  return base::OkStatus();
}

// Replaces every alias with one covering the whole row as it is now.
base::Status Compiler::CompileAs(uint32_t stage) {
  scope_.op = "AS";
  const auto* n = Node<SyntaqlitePerfettoPipeAs>(p_, stage);
  scope_.aliases.clear();
  scope_.aliases.push_back({SpanText(p_, n->alias), scope_.row});
  return base::OkStatus();
}

base::Status Compiler::CompilePipeline(uint32_t pipeline) {
  const auto& n = Node<SyntaqliteNode>(p_, pipeline)->perfetto_pipeline;
  if (syntaqlite_node_is_present(n.intersection)) {
    RETURN_IF_ERROR(CompileIntersection(n.intersection));
  } else {
    RETURN_IF_ERROR(CompileSource(n.from));
  }
  if (!syntaqlite_node_is_present(n.stages)) {
    return base::OkStatus();
  }
  const auto* stages = Node<SyntaqlitePerfettoPipeStageList>(p_, n.stages);
  uint32_t count = syntaqlite_list_count(stages);
  for (uint32_t i = 0; i < count; i++) {
    RETURN_IF_ERROR(CompileStage(syntaqlite_list_child_id(stages, i)));
  }
  return base::OkStatus();
}

LogicalPlan Compiler::Finish() {
  plan_.output.clear();
  for (const RowColumn& column : scope_.row) {
    plan_.output.push_back(column.column);
  }
  return std::move(plan_);
}

}  // namespace

base::StatusOr<LogicalPlan> Compile(SyntaqliteParser* p,
                                    uint32_t pipeline,
                                    const NodeSourceFn& source,
                                    const Catalog& catalog) {
  Compiler compiler(p, source, catalog);
  RETURN_IF_ERROR(compiler.CompilePipeline(pipeline));
  LogicalPlan plan = compiler.Finish();
  PushDownFilters(plan);
  PruneColumns(plan);
  return plan;
}

}  // namespace perfetto::trace_processor::pipeline
