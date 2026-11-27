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

#include "src/trace_processor/perfetto_sql/pipesql/stages.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipesql/expressions.h"

namespace perfetto::trace_processor::pipesql {
namespace {

using pipeline::Builder;
using pipeline::ColumnId;
using pipeline::Expr;
using pipeline::NameId;
using pipeline::PlanType;

base::StatusOr<std::vector<Translator::RowColumn>>
ExpandStar(Translator*, uint32_t star_id, bool allow_unqualified_star);

bool IsColumn(Translator* t, uint32_t expr) {
  return Node<SyntaqliteNode>(t->parser(), expr)->tag ==
         SYNTAQLITE_NODE_COLUMN_REF;
}

base::StatusOr<std::optional<ColumnId>> ResolveValue(
    Translator* t,
    uint32_t expr,
    NameId name,
    std::vector<Builder::ComputeItem>* pending) {
  if (IsColumn(t, expr)) {
    ASSIGN_OR_RETURN(ColumnId id, t->ResolveColumnExpr(expr));
    return std::optional<ColumnId>(id);
  }
  Expr e;
  RETURN_IF_ERROR(TranslateExpr(t, expr, &e));
  base::StatusOr<PlanType> type = t->builder().CheckExprType(e);
  if (!type.ok()) {
    return t->Expected(expr, type.status().message());
  }
  pending->push_back({name, *type, e});
  return std::optional<ColumnId>(std::nullopt);
}

base::StatusOr<std::vector<Translator::RowColumn>>
ResolveItems(Translator* t, uint32_t list_id, bool allow_unqualified_star) {
  const auto* list =
      Node<SyntaqlitePerfettoPipeSelectItemList>(t->parser(), list_id);
  std::vector<Translator::RowColumn> columns;
  std::vector<Builder::ComputeItem> pending;
  std::vector<size_t> computed_slots;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    if (Node<SyntaqliteNode>(t->parser(), item_id)->tag ==
        SYNTAQLITE_NODE_PERFETTO_PIPE_STAR) {
      ASSIGN_OR_RETURN(std::vector<Translator::RowColumn> expanded,
                       ExpandStar(t, item_id, allow_unqualified_star));
      columns.insert(columns.end(), expanded.begin(), expanded.end());
      continue;
    }
    const auto* item =
        Node<SyntaqlitePerfettoPipeExprItem>(t->parser(), item_id);
    NameId name;
    if (IsSpanPresent(item->alias)) {
      name = t->Name(item->alias);
    } else if (IsColumn(t, item->expr)) {
      name = t->Name(
          Node<SyntaqliteNode>(t->parser(), item->expr)->column_ref.column);
    } else {
      return t->Expected(item->expr,
                         "a name for the computed column, as in `x + 1 AS y`");
    }
    ASSIGN_OR_RETURN(std::optional<ColumnId> id,
                     ResolveValue(t, item->expr, name, &pending));
    if (!id) {
      computed_slots.push_back(columns.size());
    }
    columns.push_back({{name, id.value_or(0)}, t->operation(), item_id});
  }
  if (!pending.empty()) {
    auto built = t->builder().Compute(t->root(), std::move(pending));
    t->SetRoot(built.relation);
    for (size_t i = 0; i < computed_slots.size(); ++i) {
      columns[computed_slots[i]].column.id = built.columns[i];
    }
  }
  return columns;
}

base::StatusOr<std::vector<Translator::RowColumn>>
ExpandStar(Translator* t, uint32_t star_id, bool allow_unqualified_star) {
  const auto* star = Node<SyntaqlitePerfettoPipeStar>(t->parser(), star_id);
  std::vector<Translator::RowColumn> columns;
  if (IsSpanPresent(star->qualifier)) {
    NameId qualifier = t->Name(star->qualifier);
    const Translator::Alias* alias = t->FindAlias(qualifier);
    if (!alias) {
      return t->Err(star_id, Translator::Error::kNoSuchAlias,
                    t->Text(qualifier));
    }
    columns = alias->columns;
  } else {
    if (!allow_unqualified_star) {
      return t->Expected(star_id, "a table alias before the star, as in `t.*`");
    }
    for (const Translator::RowColumn& column : t->row()) {
      if (!column.qualified_only) {
        columns.push_back(column);
      }
    }
  }

  if (syntaqlite_node_is_present(star->except)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeNameList>(t->parser(), star->except);
    std::vector<NameId> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      NameId name =
          t->Name(Node<SyntaqlitePerfettoPipeName>(t->parser(), item_id)->name);
      RETURN_IF_ERROR(t->CheckListedOnce(seen, name, item_id));
      auto matches = [&](const Translator::RowColumn& column) {
        return t->Same(column.column.name, name);
      };
      auto it = std::remove_if(columns.begin(), columns.end(), matches);
      if (it == columns.end()) {
        return t->Err(item_id, Translator::Error::kNoSuchColumn, t->Text(name));
      }
      columns.erase(it, columns.end());
    }
    if (columns.empty()) {
      return t->Expected(star_id, "a column to be left after EXCEPT");
    }
  }

  if (syntaqlite_node_is_present(star->replace)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeColumnList>(t->parser(), star->replace);
    std::vector<NameId> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      const auto* item =
          Node<SyntaqlitePerfettoPipeColumn>(t->parser(), item_id);
      NameId target = t->Name(item->alias);
      RETURN_IF_ERROR(t->CheckListedOnce(seen, target, item_id));
      std::vector<Translator::RowColumn*> matches;
      for (Translator::RowColumn& column : columns) {
        if (t->Same(column.column.name, target)) {
          matches.push_back(&column);
        }
      }
      if (matches.empty()) {
        return t->Err(item_id, Translator::Error::kNoSuchColumn,
                      t->Text(target));
      }
      if (matches.size() > 1) {
        return t->Err(item_id, Translator::Error::kAmbiguousColumn,
                      t->Text(target),
                      t->AmbiguousCandidates({matches.begin(), matches.end()}));
      }
      std::optional<NameId> qualifier;
      if (IsSpanPresent(item->qualifier)) {
        qualifier = t->Name(item->qualifier);
      }
      ASSIGN_OR_RETURN(ColumnId value,
                       t->Resolve(qualifier, t->Name(item->name), item_id));
      *matches.front() = {{target, value}, t->operation(), item_id};
    }
  }

  for (Translator::RowColumn& column : columns) {
    column.qualified_only = false;
  }
  return columns;
}

}  // namespace

base::Status TranslateSelect(Translator* t, uint32_t stage) {
  t->SetOperation("SELECT");
  const auto* n = Node<SyntaqlitePerfettoPipeSelect>(t->parser(), stage);
  ASSIGN_OR_RETURN(std::vector<Translator::RowColumn> row,
                   ResolveItems(t, n->columns, true));
  t->ReplaceRow(std::move(row));
  t->ClearAliases();
  return base::OkStatus();
}

base::Status TranslateExtend(Translator* t, uint32_t stage) {
  t->SetOperation("EXTEND");
  const auto* n = Node<SyntaqlitePerfettoPipeExtend>(t->parser(), stage);
  ASSIGN_OR_RETURN(std::vector<Translator::RowColumn> columns,
                   ResolveItems(t, n->columns, false));
  t->ExtendRow(std::move(columns));
  return base::OkStatus();
}

base::Status TranslateDrop(Translator* t, uint32_t stage) {
  t->SetOperation("DROP");
  const auto* n = Node<SyntaqlitePerfettoPipeDrop>(t->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeNameList>(t->parser(), n->columns);
  std::vector<NameId> names;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    NameId name =
        t->Name(Node<SyntaqlitePerfettoPipeName>(t->parser(), item_id)->name);
    RETURN_IF_ERROR(t->CheckListedOnce(names, name, item_id));
    bool found =
        std::any_of(t->row().begin(), t->row().end(), [&](const auto& column) {
          return !column.qualified_only && t->Same(column.column.name, name);
        });
    if (!found) {
      return t->NoSuchColumn(name, item_id);
    }
  }
  for (NameId name : names) {
    t->DropColumn(name);
  }
  if (t->row().empty()) {
    return t->Expected(stage, "a column to be left after DROP");
  }
  return base::OkStatus();
}

base::Status TranslateRename(Translator* t, uint32_t stage) {
  t->SetOperation("RENAME");
  const auto* n = Node<SyntaqlitePerfettoPipeRename>(t->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeColumnList>(t->parser(), n->columns);
  std::vector<NameId> seen;
  std::vector<std::pair<size_t, uint32_t>> renames;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(t->parser(), item_id);
    NameId name = t->Name(item->name);
    RETURN_IF_ERROR(t->CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, t->FindInRow(name, item_id));
    renames.push_back({at, item_id});
  }
  for (const auto& [at, item_id] : renames) {
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(t->parser(), item_id);
    t->RenameColumn(at, t->Name(item->alias), item_id);
  }
  return base::OkStatus();
}

base::Status TranslateSet(Translator* t, uint32_t stage) {
  t->SetOperation("SET");
  const auto* n = Node<SyntaqlitePerfettoPipeSet>(t->parser(), stage);
  const auto* list =
      Node<SyntaqlitePerfettoPipeSetItemList>(t->parser(), n->items);
  std::vector<NameId> seen;
  struct Assignment {
    size_t at;
    std::optional<ColumnId> value;
    uint32_t node;
  };
  std::vector<Assignment> assignments;
  std::vector<Builder::ComputeItem> pending;
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item =
        Node<SyntaqlitePerfettoPipeSetItem>(t->parser(), item_id);
    NameId name = t->Name(item->name);
    RETURN_IF_ERROR(t->CheckListedOnce(seen, name, item_id));
    ASSIGN_OR_RETURN(size_t at, t->FindInRow(name, item_id));
    ASSIGN_OR_RETURN(std::optional<ColumnId> id,
                     ResolveValue(t, item->value, name, &pending));
    assignments.push_back({at, id, item_id});
  }
  std::vector<ColumnId> computed;
  if (!pending.empty()) {
    auto built = t->builder().Compute(t->root(), std::move(pending));
    t->SetRoot(built.relation);
    computed = std::move(built.columns);
  }
  size_t next_computed = 0;
  for (const Assignment& assignment : assignments) {
    ColumnId value =
        assignment.value ? *assignment.value : computed[next_computed++];
    t->SetColumnValue(assignment.at, value, assignment.node);
  }
  return base::OkStatus();
}

base::Status TranslateAs(Translator* t, uint32_t stage) {
  t->SetOperation("AS");
  const auto* n = Node<SyntaqlitePerfettoPipeAs>(t->parser(), stage);
  std::vector<Translator::RowColumn> row;
  for (const Translator::RowColumn& column : t->row()) {
    if (!column.qualified_only) {
      row.push_back(column);
    }
  }
  t->ClearAliases();
  t->AddAlias({t->Name(n->alias), row});
  t->ReplaceRow(std::move(row));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipesql
