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

namespace perfetto::trace_processor::pipeline {

// The stages below are relational operators which only change which columns
// the row has and what they are called: nothing runs. Where an operator could
// take an expression, it takes a column reference for now.

const OperationRegistration Projection::kSelect{
    SYNTAQLITE_NODE_PERFETTO_PIPE_SELECT, &BuildSelectPlan};
const OperationRegistration Projection::kExtend{
    SYNTAQLITE_NODE_PERFETTO_PIPE_EXTEND, &BuildExtendPlan};
const OperationRegistration Projection::kDrop{
    SYNTAQLITE_NODE_PERFETTO_PIPE_DROP, &BuildDropPlan};
const OperationRegistration Projection::kRename{
    SYNTAQLITE_NODE_PERFETTO_PIPE_RENAME, &BuildRenamePlan};
const OperationRegistration Projection::kSet{SYNTAQLITE_NODE_PERFETTO_PIPE_SET,
                                             &BuildSetPlan};
const OperationRegistration Projection::kAs{SYNTAQLITE_NODE_PERFETTO_PIPE_AS,
                                            &BuildAsPlan};

// Replaces the row. What it leaves is a new table: no alias reaches into it.
base::Status Projection::BuildSelectPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("SELECT");
  const auto* n = Node<SyntaqlitePerfettoPipeSelect>(c->parser(), stage);
  ASSIGN_OR_RETURN(std::vector<Compiler::RowColumn> row,
                   Projection::ResolveItems(c, n->columns, true));
  c->SetRow(std::move(row));
  c->ClearAliases();
  return base::OkStatus();
}

// Adds columns to the row. Items see only the row before the stage, not each
// other.
base::Status Projection::BuildExtendPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("EXTEND");
  const auto* n = Node<SyntaqlitePerfettoPipeExtend>(c->parser(), stage);
  ASSIGN_OR_RETURN(std::vector<Compiler::RowColumn> columns,
                   Projection::ResolveItems(c, n->columns, false));
  c->ExtendRow(std::move(columns));
  return base::OkStatus();
}

// Removes every column of each name. Aliases still reach the dropped columns,
// except an alias of the same name, which the name now hides.
base::Status Projection::BuildDropPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("DROP");
  const auto* n = Node<SyntaqlitePerfettoPipeDrop>(c->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeNameList>(c->parser(), n->columns);
  std::vector<std::string> names;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    std::string name =
        SpanText(c->parser(),
                 Node<SyntaqlitePerfettoPipeName>(c->parser(), item_id)->name);
    RETURN_IF_ERROR(c->CheckListedOnce(names, name, item_id));
    bool found =
        std::any_of(c->row().begin(), c->row().end(), [&](const auto& column) {
          return !column.qualified_only &&
                 base::CaseInsensitiveEqual(column.column.name, name);
        });
    if (!found) {
      return c->NoSuchColumn(name, item_id);
    }
  }
  for (const std::string& name : names) {
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
base::Status Projection::BuildRenamePlan(Compiler* c, uint32_t stage) {
  c->SetOperation("RENAME");
  const auto* n = Node<SyntaqlitePerfettoPipeRename>(c->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeColumnList>(c->parser(), n->columns);
  std::vector<std::string> seen;
  std::vector<std::pair<size_t, uint32_t>> renames;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
    std::string name = SpanText(c->parser(), item->name);
    RETURN_IF_ERROR(c->CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, c->FindInRow(name, item_id));
    renames.push_back({at, item_id});
  }
  for (const auto& [at, item_id] : renames) {
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
    c->RenameColumn(at, SpanText(c->parser(), item->alias), item_id);
  }
  return base::OkStatus();
}

// Replaces the values of columns in place. Each name must find exactly one
// column, and every value is read from the row before the stage. Aliases
// still reach the old values, except an alias of the same name, which the
// name now hides.
base::Status Projection::BuildSetPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("SET");
  const auto* n = Node<SyntaqlitePerfettoPipeSet>(c->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeSetItemList>(c->parser(), n->items);
  std::vector<std::string> seen;
  struct Assignment {
    size_t at;
    ColumnId value;
    uint32_t node;
  };
  std::vector<Assignment> assignments;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item =
        Node<SyntaqlitePerfettoPipeSetItem>(c->parser(), item_id);
    std::string name = SpanText(c->parser(), item->name);
    RETURN_IF_ERROR(c->CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, c->FindInRow(name, item_id));
    const auto* value =
        Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item->value);
    std::string qualifier = IsPresent(value->qualifier)
                                ? SpanText(c->parser(), value->qualifier)
                                : "";
    ASSIGN_OR_RETURN(
        ColumnId id,
        c->Resolve(qualifier, SpanText(c->parser(), value->name), item->value));
    assignments.push_back({at, id, item_id});
  }
  for (const Assignment& assignment : assignments) {
    c->SetColumnValue(assignment.at, assignment.value, assignment.node);
  }
  return base::OkStatus();
}

// Replaces every alias with one covering the whole row as it is now.
base::Status Projection::BuildAsPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("AS");
  const auto* n = Node<SyntaqlitePerfettoPipeAs>(c->parser(), stage);
  c->ClearAliases();
  c->AddAlias({SpanText(c->parser(), n->alias), c->row()});
  return base::OkStatus();
}

base::StatusOr<std::vector<Compiler::RowColumn>> Projection::ResolveItems(
    Compiler* c,
    uint32_t list_id,
    bool allow_unqualified_star) {
  const auto* list =
      Node<SyntaqlitePerfettoPipeSelectItemList>(c->parser(), list_id);
  std::vector<Compiler::RowColumn> columns;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    if (Node<SyntaqliteNode>(c->parser(), item_id)->tag ==
        SYNTAQLITE_NODE_PERFETTO_PIPE_STAR) {
      ASSIGN_OR_RETURN(
          std::vector<Compiler::RowColumn> expanded,
          Projection::ExpandStar(c, item_id, allow_unqualified_star));
      columns.insert(columns.end(), expanded.begin(), expanded.end());
      continue;
    }
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
    std::string qualifier = IsPresent(item->qualifier)
                                ? SpanText(c->parser(), item->qualifier)
                                : "";
    std::string name = SpanText(c->parser(), item->name);
    ASSIGN_OR_RETURN(ColumnId id, c->Resolve(qualifier, name, item_id));
    if (IsPresent(item->alias)) {
      name = SpanText(c->parser(), item->alias);
    }
    columns.push_back({{std::move(name), id}, c->operation(), item_id});
  }
  return columns;
}

base::StatusOr<std::vector<Compiler::RowColumn>> Projection::ExpandStar(
    Compiler* c,
    uint32_t star_id,
    bool allow_unqualified_star) {
  const auto* star = Node<SyntaqlitePerfettoPipeStar>(c->parser(), star_id);
  std::vector<Compiler::RowColumn> columns;
  if (IsPresent(star->qualifier)) {
    std::string qualifier = SpanText(c->parser(), star->qualifier);
    const Compiler::Alias* alias = c->FindAlias(qualifier);
    if (!alias) {
      return c->Err(star_id, Compiler::Error::kNoSuchAlias, qualifier);
    }
    columns = alias->columns;
  } else {
    // EXTEND only takes `alias.*`: a bare star would repeat the whole row.
    if (!allow_unqualified_star) {
      return c->Expected(star_id, "a table alias before the star, as in `t.*`");
    }
    columns = c->row();
  }

  if (syntaqlite_node_is_present(star->except)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeNameList>(c->parser(), star->except);
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      std::string name = SpanText(
          c->parser(),
          Node<SyntaqlitePerfettoPipeName>(c->parser(), item_id)->name);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, name, item_id));
      // Every column of that name goes.
      auto matches = [&](const Compiler::RowColumn& column) {
        return base::CaseInsensitiveEqual(column.column.name, name);
      };
      auto it = std::remove_if(columns.begin(), columns.end(), matches);
      if (it == columns.end()) {
        return c->Err(item_id, Compiler::Error::kNoSuchColumn, name);
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
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      const auto* item =
          Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
      std::string target = SpanText(c->parser(), item->alias);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, target, item_id));
      std::vector<Compiler::RowColumn*> matches;
      for (Compiler::RowColumn& column : columns) {
        if (base::CaseInsensitiveEqual(column.column.name, target)) {
          matches.push_back(&column);
        }
      }
      if (matches.empty()) {
        return c->Err(item_id, Compiler::Error::kNoSuchColumn, target);
      }
      if (matches.size() > 1) {
        return c->Err(item_id, Compiler::Error::kAmbiguousColumn, target,
                      c->AmbiguousCandidates({matches.begin(), matches.end()}));
      }
      std::string qualifier = IsPresent(item->qualifier)
                                  ? SpanText(c->parser(), item->qualifier)
                                  : "";
      ASSIGN_OR_RETURN(
          ColumnId value,
          c->Resolve(qualifier, SpanText(c->parser(), item->name), item_id));
      *matches.front() = {{std::move(target), value}, c->operation(), item_id};
    }
  }

  // Whatever a star lists is a column of the new row like any other.
  for (Compiler::RowColumn& column : columns) {
    column.qualified_only = false;
  }
  return columns;
}

}  // namespace perfetto::trace_processor::pipeline
