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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COMPILER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COMPILER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

struct SyntaqliteParser;

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// Compilation entry point
// -----------------------------------------------------------------------------

// Source text of a parse tree node, for error tracebacks.
using NodeSourceFn = std::function<SqlSource(uint32_t node)>;

// Compiles a parsed pipeline into a logical plan, resolving the source and
// column names against `catalog` and checking types where known. Runs while
// the parse tree is alive so unsupported queries fail at parse time.
//
// `pipeline` is the SYNTAQLITE_NODE_PERFETTO_PIPELINE node.
base::StatusOr<LogicalPlan> Compile(SyntaqliteParser*,
                                    uint32_t pipeline,
                                    const NodeSourceFn&,
                                    const Catalog&);

// -----------------------------------------------------------------------------
// AST helpers
// -----------------------------------------------------------------------------

// The name `span` spells. A quoted name escapes its closing quote by doubling
// it, which the span, pointing into the source, still contains.
std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span);

// Whether `span` was written at all. An empty quoted name, like `""`, has
// no length but is still there.
bool IsSpanPresent(SyntaqliteTextSpan span);

template <typename T>
const T* Node(SyntaqliteParser* p, uint32_t id) {
  return static_cast<const T*>(syntaqlite_parser_node(p, id));
}

// -----------------------------------------------------------------------------
// Plan construction
// -----------------------------------------------------------------------------

// Interface used by operation builders to turn a pipeline AST into a logical
// plan. Owns the plan and the current name scope: adding a plan node does not
// change the visible row, and changing the row does not add execution work.
// Builders must update both as appropriate for their operation.
// The parser, source callback and catalog are borrowed until compilation ends.
class Compiler {
 public:
  // ---------------------------------------------------------------------------
  // Scope and diagnostics types
  // ---------------------------------------------------------------------------

  // A column of the row a stage produces, and where it came from.
  struct RowColumn {
    NamedColumn column;
    // The operation and AST node which put it here, for error messages.
    const char* operation;
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

  // Every error is one of a few shapes filled in with a short noun, so a new
  // check costs one call and one short literal.
  enum class Error {
    kExpected,
    kUnsupported,
    kNoSuchColumn,
    kAmbiguousColumn,
    kNoSuchAlias,
  };

  // ---------------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------------

  Compiler(SyntaqliteParser* p, const NodeSourceFn& source, const Catalog& c);

  // Compiles the pipeline at `pipeline`: its source and each of its stages.
  base::Status CompilePipeline(uint32_t pipeline);

  // Sets the output to the current row and transfers the completed plan.
  LogicalPlan Finish();

  // ---------------------------------------------------------------------------
  // Logical plan construction
  // ---------------------------------------------------------------------------

  // Allocates a logical value without making a name visible in the row.
  ColumnId AddColumn(std::string name, std::optional<core::StorageType> type);

  // Appends an operation and makes it the plan root; does not update scope.
  template <typename T>
  PlanNodeId AddNode(T operation, std::vector<PlanNodeId> children = {}) {
    return plan_.AddNode(std::move(operation), std::move(children));
  }

  // ---------------------------------------------------------------------------
  // Name resolution and expression checks
  // ---------------------------------------------------------------------------

  const Alias* FindAlias(const std::string& name) const;

  // The position in the row of the one column a bare `name` finds.
  base::StatusOr<size_t> FindInRow(const std::string& name, uint32_t at) const;

  // The error for a bare `name` which finds no column.
  base::Status NoSuchColumn(const std::string& name, uint32_t at) const;

  // Fails if a name appears twice in a list of names.
  base::Status CheckListedOnce(std::vector<std::string>& seen,
                               const std::string& name,
                               uint32_t at) const;

  // Aggregate argument checks and column resolution.
  bool IsCountStar(uint32_t expr) const;
  base::StatusOr<ColumnId> ResolveSum(uint32_t expr);
  // Resolves an expression which must be a column reference; any other
  // expression is unsupported.
  base::StatusOr<ColumnId> ResolveColumnExpr(uint32_t expr) const;
  base::StatusOr<ColumnId> Resolve(const std::string& name, uint32_t at) const;
  base::StatusOr<ColumnId> Resolve(const std::string& qualifier,
                                   const std::string& name,
                                   uint32_t at) const;

  // ---------------------------------------------------------------------------
  // Diagnostics
  // ---------------------------------------------------------------------------

  SqlSource SourceForNode(uint32_t node) const;

  base::Status Err(uint32_t at,
                   Error,
                   std::string_view what,
                   const std::string& detail = "") const;
  base::Status Expected(uint32_t at, std::string_view what) const;
  base::Status Unsupported(uint32_t at, std::string_view what) const;
  std::string AmbiguousCandidates(
      const std::vector<const RowColumn*>& matches) const;

  // ---------------------------------------------------------------------------
  // Row and alias updates
  // ---------------------------------------------------------------------------

  // Diagnostic label for subsequent scope changes; must outlive compilation.
  void SetOperation(const char* operation);

  // Replaces the whole visible row. Aliases are retained unless ClearAliases
  // is used.
  void ReplaceRow(std::vector<RowColumn> row);
  void ExtendRow(std::vector<RowColumn> columns);

  // Removes matching unqualified columns and the alias with this name.
  void DropColumn(const std::string& name);

  // Changes a row binding, preserving earlier alias snapshots. `at` is a row
  // index; `node` records the AST location for later diagnostics.
  void RenameColumn(size_t at, std::string name, uint32_t node);

  // Rebinds a row position and removes the alias with that column name.
  void SetColumnValue(size_t at, ColumnId value, uint32_t node);
  // Appends a column to the row.
  void Append(NamedColumn column, uint32_t node, bool qualified_only = false);

  void ClearAliases();
  void AddAlias(Alias alias);

  // Points each alias at the column replacing each one it names, as `from` to
  // `to` in `replaced`, for a stage which gives every column new values. A
  // column with no replacement drops out of the alias.
  void ReplaceAliasColumns(
      const std::vector<std::pair<ColumnId, ColumnId>>& replaced);

  // Forgets the table alias `name`, if there is one.
  void RemoveAlias(const std::string& name);

  // ---------------------------------------------------------------------------
  // Inputs and current scope
  // ---------------------------------------------------------------------------

  SyntaqliteParser* parser() const { return p_; }
  const Catalog& catalog() const { return catalog_; }
  const LogicalPlan& plan() const { return plan_; }
  const std::vector<RowColumn>& row() const { return scope_.row; }
  const char* operation() const { return scope_.operation; }

 private:
  // What names mean while a pipeline is compiled.
  struct Scope {
    // Diagnostic label for the current operation; it does not control behavior.
    const char* operation = "FROM";
    // The row as of the stage being compiled, and the table aliases in scope.
    std::vector<RowColumn> row;
    std::vector<Alias> aliases;
  };

  // Builds a source or stage from its AST node ID. A projection can update
  // scope without adding a logical plan node.
  base::Status BuildOperation(uint32_t stage);

  // How a user could write a reference to `column`, to tell candidates apart.
  std::string Origin(const RowColumn& column) const;
  std::string Traceback(uint32_t node) const;

  // ---------------------------------------------------------------------------
  // Shared compiler state
  // ---------------------------------------------------------------------------

  SyntaqliteParser* p_;
  const NodeSourceFn& source_;
  const Catalog& catalog_;

  // The plan under construction and the names visible at its current root.
  LogicalPlan plan_;
  Scope scope_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COMPILER_H_
