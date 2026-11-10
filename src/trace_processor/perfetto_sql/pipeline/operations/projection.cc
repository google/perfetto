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

#include "src/trace_processor/perfetto_sql/pipeline/operations/projection.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/compute.h"

namespace perfetto::trace_processor::pipeline::projection {
namespace {

// The stages below change which columns the row has and what they are called.
// An item which is just a column runs nothing; any other expression is added
// to a Compute.

// The columns a SELECT or EXTEND list produces, resolved against the row
// before the stage. Computed ones are added to `computed`.
base::StatusOr<std::vector<Compiler::RowColumn>> ResolveItems(
    Compiler*,
    uint32_t list_id,
    bool allow_unqualified_star,
    std::vector<Compute::Item>* computed);
base::StatusOr<std::vector<Compiler::RowColumn>>
ExpandStar(Compiler*, uint32_t star_id, bool allow_unqualified_star);

bool IsColumn(Compiler* c, uint32_t expr) {
  return Node<SyntaqliteNode>(c->parser(), expr)->tag ==
         SYNTAQLITE_NODE_COLUMN_REF;
}

// The column an item's expression names, or else a new column it computes,
// named `name`, added to `computed`.
base::StatusOr<ColumnId> ResolveValue(Compiler* c,
                                      uint32_t expr,
                                      NameId name,
                                      std::vector<Compute::Item>* computed) {
  if (IsColumn(c, expr)) {
    return c->ResolveColumnExpr(expr);
  }
  Expr e;
  RETURN_IF_ERROR(CompileExpr(c, expr, &e));
  base::StatusOr<PlanType> type = CheckExprType(*c, e);
  if (!type.ok()) {
    return c->Expected(expr, type.status().message());
  }
  ColumnId id = c->AddColumn(name, *type);
  computed->push_back({id, std::move(e)});
  return id;
}

// Adds the Compute for a stage's computed columns, if it has any.
void AddCompute(Compiler* c, std::vector<Compute::Item> computed) {
  if (!computed.empty()) {
    c->AddNode(Compute(std::move(computed)), {c->plan().root()});
  }
}

// Replaces the row. What it leaves is a new table: no alias reaches into it.
base::Status BuildSelectPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("SELECT");
  const auto* n = Node<SyntaqlitePerfettoPipeSelect>(c->parser(), stage);
  std::vector<Compute::Item> computed;
  ASSIGN_OR_RETURN(std::vector<Compiler::RowColumn> row,
                   ResolveItems(c, n->columns, true, &computed));
  AddCompute(c, std::move(computed));
  c->ReplaceRow(std::move(row));
  c->ClearAliases();
  return base::OkStatus();
}

// Adds columns to the row. Items see only the row before the stage, not each
// other.
base::Status BuildExtendPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("EXTEND");
  const auto* n = Node<SyntaqlitePerfettoPipeExtend>(c->parser(), stage);
  std::vector<Compute::Item> computed;
  ASSIGN_OR_RETURN(std::vector<Compiler::RowColumn> columns,
                   ResolveItems(c, n->columns, false, &computed));
  AddCompute(c, std::move(computed));
  c->ExtendRow(std::move(columns));
  return base::OkStatus();
}

// Removes every column of each name. Aliases still reach the dropped columns,
// except an alias of the same name, which the name now hides.
base::Status BuildDropPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("DROP");
  const auto* n = Node<SyntaqlitePerfettoPipeDrop>(c->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeNameList>(c->parser(), n->columns);
  std::vector<NameId> names;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    NameId name =
        c->Name(Node<SyntaqlitePerfettoPipeName>(c->parser(), item_id)->name);
    RETURN_IF_ERROR(c->CheckListedOnce(names, name, item_id));
    bool found =
        std::any_of(c->row().begin(), c->row().end(), [&](const auto& column) {
          return !column.qualified_only && c->Same(column.column.name, name);
        });
    if (!found) {
      return c->NoSuchColumn(name, item_id);
    }
  }
  for (NameId name : names) {
    c->DropColumn(name);
  }
  if (c->row().empty()) {
    return c->Expected(stage, "a column to be left after DROP");
  }
  return base::OkStatus();
}

// Renames columns in place. Each name must find exactly one column, and all
// renames happen at once, so two columns can swap names. Aliases still reach
// the columns under their old names.
base::Status BuildRenamePlan(Compiler* c, uint32_t stage) {
  c->SetOperation("RENAME");
  const auto* n = Node<SyntaqlitePerfettoPipeRename>(c->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeColumnList>(c->parser(), n->columns);
  std::vector<NameId> seen;
  std::vector<std::pair<size_t, uint32_t>> renames;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
    NameId name = c->Name(item->name);
    RETURN_IF_ERROR(c->CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, c->FindInRow(name, item_id));
    renames.push_back({at, item_id});
  }
  for (const auto& [at, item_id] : renames) {
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
    c->RenameColumn(at, c->Name(item->alias), item_id);
  }
  return base::OkStatus();
}

// Replaces the values of columns in place. Each name must find exactly one
// column, and every value is read from the row before the stage. Aliases
// still reach the old values, except an alias of the same name, which the
// name now hides.
base::Status BuildSetPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("SET");
  const auto* n = Node<SyntaqlitePerfettoPipeSet>(c->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeSetItemList>(c->parser(), n->items);
  std::vector<NameId> seen;
  struct Assignment {
    size_t at;
    ColumnId value;
    uint32_t node;
  };
  std::vector<Assignment> assignments;
  std::vector<Compute::Item> computed;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item =
        Node<SyntaqlitePerfettoPipeSetItem>(c->parser(), item_id);
    NameId name = c->Name(item->name);
    RETURN_IF_ERROR(c->CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, c->FindInRow(name, item_id));
    ASSIGN_OR_RETURN(ColumnId id,
                     ResolveValue(c, item->value, name, &computed));
    assignments.push_back({at, id, item_id});
  }
  AddCompute(c, std::move(computed));
  for (const Assignment& assignment : assignments) {
    c->SetColumnValue(assignment.at, assignment.value, assignment.node);
  }
  return base::OkStatus();
}

// Replaces every alias with one covering the row as a bare name sees it now.
// Columns which only an earlier alias found go with that alias.
base::Status BuildAsPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("AS");
  const auto* n = Node<SyntaqlitePerfettoPipeAs>(c->parser(), stage);
  std::vector<Compiler::RowColumn> row;
  for (const Compiler::RowColumn& column : c->row()) {
    if (!column.qualified_only) {
      row.push_back(column);
    }
  }
  c->ClearAliases();
  c->AddAlias({c->Name(n->alias), row});
  c->ReplaceRow(std::move(row));
  return base::OkStatus();
}

base::StatusOr<std::vector<Compiler::RowColumn>> ResolveItems(
    Compiler* c,
    uint32_t list_id,
    bool allow_unqualified_star,
    std::vector<Compute::Item>* computed) {
  const auto* list =
      Node<SyntaqlitePerfettoPipeSelectItemList>(c->parser(), list_id);
  std::vector<Compiler::RowColumn> columns;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    if (Node<SyntaqliteNode>(c->parser(), item_id)->tag ==
        SYNTAQLITE_NODE_PERFETTO_PIPE_STAR) {
      ASSIGN_OR_RETURN(std::vector<Compiler::RowColumn> expanded,
                       ExpandStar(c, item_id, allow_unqualified_star));
      columns.insert(columns.end(), expanded.begin(), expanded.end());
      continue;
    }
    const auto* item =
        Node<SyntaqlitePerfettoPipeExprItem>(c->parser(), item_id);
    NameId name;
    if (IsSpanPresent(item->alias)) {
      name = c->Name(item->alias);
    } else if (IsColumn(c, item->expr)) {
      name = c->Name(
          Node<SyntaqliteNode>(c->parser(), item->expr)->column_ref.column);
    } else {
      return c->Expected(item->expr,
                         "a name for the computed column, as in `x + 1 AS y`");
    }
    ASSIGN_OR_RETURN(ColumnId id, ResolveValue(c, item->expr, name, computed));
    columns.push_back({{name, id}, c->operation(), item_id});
  }
  return columns;
}

base::StatusOr<std::vector<Compiler::RowColumn>>
ExpandStar(Compiler* c, uint32_t star_id, bool allow_unqualified_star) {
  const auto* star = Node<SyntaqlitePerfettoPipeStar>(c->parser(), star_id);
  std::vector<Compiler::RowColumn> columns;
  if (IsSpanPresent(star->qualifier)) {
    NameId qualifier = c->Name(star->qualifier);
    const Compiler::Alias* alias = c->FindAlias(qualifier);
    if (!alias) {
      return c->Err(star_id, Compiler::Error::kNoSuchAlias, c->Text(qualifier));
    }
    columns = alias->columns;
  } else {
    // EXTEND only takes `alias.*`: a bare star would repeat the whole row.
    if (!allow_unqualified_star) {
      return c->Expected(star_id, "a table alias before the star, as in `t.*`");
    }
    // A bare star is the columns a bare name finds. Others, as an
    // intersection's operand columns are, come through their alias.
    for (const Compiler::RowColumn& column : c->row()) {
      if (!column.qualified_only) {
        columns.push_back(column);
      }
    }
  }

  if (syntaqlite_node_is_present(star->except)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeNameList>(c->parser(), star->except);
    std::vector<NameId> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      NameId name =
          c->Name(Node<SyntaqlitePerfettoPipeName>(c->parser(), item_id)->name);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, name, item_id));
      // Every column of that name goes.
      auto matches = [&](const Compiler::RowColumn& column) {
        return c->Same(column.column.name, name);
      };
      auto it = std::remove_if(columns.begin(), columns.end(), matches);
      if (it == columns.end()) {
        return c->Err(item_id, Compiler::Error::kNoSuchColumn, c->Text(name));
      }
      columns.erase(it, columns.end());
    }
    if (columns.empty()) {
      return c->Expected(star_id, "a column to be left after EXCEPT");
    }
  }

  if (syntaqlite_node_is_present(star->replace)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeColumnList>(c->parser(), star->replace);
    std::vector<NameId> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      const auto* item =
          Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
      NameId target = c->Name(item->alias);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, target, item_id));
      std::vector<Compiler::RowColumn*> matches;
      for (Compiler::RowColumn& column : columns) {
        if (c->Same(column.column.name, target)) {
          matches.push_back(&column);
        }
      }
      if (matches.empty()) {
        return c->Err(item_id, Compiler::Error::kNoSuchColumn, c->Text(target));
      }
      if (matches.size() > 1) {
        return c->Err(item_id, Compiler::Error::kAmbiguousColumn,
                      c->Text(target),
                      c->AmbiguousCandidates({matches.begin(), matches.end()}));
      }
      std::optional<NameId> qualifier;
      if (IsSpanPresent(item->qualifier)) {
        qualifier = c->Name(item->qualifier);
      }
      ASSIGN_OR_RETURN(ColumnId value,
                       c->Resolve(qualifier, c->Name(item->name), item_id));
      *matches.front() = {{target, value}, c->operation(), item_id};
    }
  }

  // Whatever a star lists is a column of the new row like any other.
  for (Compiler::RowColumn& column : columns) {
    column.qualified_only = false;
  }
  return columns;
}

}  // namespace

const OperationRegistration kSelect{SYNTAQLITE_NODE_PERFETTO_PIPE_SELECT,
                                    &BuildSelectPlan, std::nullopt};
const OperationRegistration kExtend{SYNTAQLITE_NODE_PERFETTO_PIPE_EXTEND,
                                    &BuildExtendPlan, std::nullopt};
const OperationRegistration kDrop{SYNTAQLITE_NODE_PERFETTO_PIPE_DROP,
                                  &BuildDropPlan, std::nullopt};
const OperationRegistration kRename{SYNTAQLITE_NODE_PERFETTO_PIPE_RENAME,
                                    &BuildRenamePlan, std::nullopt};
const OperationRegistration kSet{SYNTAQLITE_NODE_PERFETTO_PIPE_SET,
                                 &BuildSetPlan, std::nullopt};
const OperationRegistration kAs{SYNTAQLITE_NODE_PERFETTO_PIPE_AS, &BuildAsPlan,
                                std::nullopt};

}  // namespace perfetto::trace_processor::pipeline::projection
