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

#include "src/perfetto_sql/analysis/relation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/analysis/string_arena.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/perfetto_sql/syntaqlite/utils.h"

namespace perfetto::perfetto_sql::analysis {
namespace {

constexpr int kMaxDepth = 32;
struct ParserDeleter {
  void operator()(SyntaqliteParser* parser) const {
    syntaqlite_parser_destroy(parser);
  }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, ParserDeleter>;

struct OwnedView {
  std::string sql;
  ScopedParser parser;
  uint32_t root = 0;
};

// The text SQLite sees for the name at `span`, after macro expansion. A name is
// one token, so its text is a slice of the one layer it is in, which lives as
// long as the statement.
std::string_view Text(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  uint32_t len = 0;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  return text ? base::TrimWhitespace(std::string_view(text, len))
              : std::string_view();
}

const SyntaqliteNode* Node(SyntaqliteParser* p, uint32_t id) {
  if (!syntaqlite_node_is_present(id)) {
    return nullptr;
  }
  return static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, id));
}

void AppendUniqueOrigins(std::vector<ColumnOrigin>* dst,
                         const std::vector<ColumnOrigin>& src) {
  for (const ColumnOrigin& origin : src) {
    if (std::find(dst->begin(), dst->end(), origin) == dst->end()) {
      dst->push_back(origin);
    }
  }
}

std::optional<std::string_view> CommonOrigin(
    const std::vector<ColumnLineage>& columns) {
  if (columns.empty() || columns.front().origins.empty()) {
    return std::nullopt;
  }
  std::string_view first = columns.front().origins.front().relation_name;
  for (const ColumnLineage& column : columns) {
    if (column.origins.empty()) {
      return std::nullopt;
    }
    for (const ColumnOrigin& origin : column.origins) {
      if (origin.relation_name != first) {
        return std::nullopt;
      }
    }
  }
  return first;
}

bool IsNatural(SyntaqliteJoinType type) {
  return type == SYNTAQLITE_JOIN_TYPE_NATURAL_INNER ||
         type == SYNTAQLITE_JOIN_TYPE_NATURAL_LEFT ||
         type == SYNTAQLITE_JOIN_TYPE_NATURAL_RIGHT ||
         type == SYNTAQLITE_JOIN_TYPE_NATURAL_FULL;
}

// The CTE definitions of `with`, in order.
std::vector<const SyntaqliteCteDefinition*> CteDefinitions(
    SyntaqliteParser* p,
    const SyntaqliteWithClause& with) {
  std::vector<const SyntaqliteCteDefinition*> out;
  const void* list = syntaqlite_parser_node(p, with.ctes);
  uint32_t count = list ? syntaqlite_list_count(list) : 0;
  for (uint32_t i = 0; i < count; ++i) {
    const SyntaqliteNode* node = Node(p, syntaqlite_list_child_id(list, i));
    if (node && node->tag == SYNTAQLITE_NODE_CTE_DEFINITION) {
      out.push_back(&node->cte_definition);
    }
  }
  return out;
}

bool ContainsName(const std::vector<std::string_view>& names,
                  std::string_view name) {
  return std::any_of(names.begin(), names.end(), [&](std::string_view n) {
    return base::CaseInsensitiveEqual(n, name);
  });
}

}  // namespace

class RelationLineage::Storage {
 public:
  Storage(std::vector<ColumnLineage> columns, bool preserves_rows)
      : strings_(StringBytes(columns)), columns_(std::move(columns)) {
    for (ColumnLineage& column : columns_) {
      column.output_name = strings_.Append(column.output_name);
      for (ColumnOrigin& origin : column.origins) {
        origin.relation_name = strings_.Append(origin.relation_name);
        origin.column_name = strings_.Append(origin.column_name);
      }
    }
    if (preserves_rows) {
      row_origin_ = CommonOrigin(columns_);
    }
  }

  const std::vector<ColumnLineage>& columns() const { return columns_; }
  std::optional<std::string_view> row_origin() const { return row_origin_; }

 private:
  static size_t StringBytes(const std::vector<ColumnLineage>& columns) {
    size_t bytes = 0;
    for (const ColumnLineage& column : columns) {
      bytes += column.output_name.size();
      for (const ColumnOrigin& origin : column.origins) {
        bytes += origin.relation_name.size();
        bytes += origin.column_name.size();
      }
    }
    return bytes;
  }

  internal::StringArena strings_;
  std::vector<ColumnLineage> columns_;
  std::optional<std::string_view> row_origin_;
};

class RelationAnalyzer::Impl {
 public:
  explicit Impl(const Catalog& catalog) : catalog_(catalog) {}

  void Begin() {
    preserves_rows_ = true;
    views_.clear();
    leaves_.clear();
    ctes_.clear();
  }

  // The columns of the relation `name`. Its hidden columns, if any, are added
  // to `hidden`.
  base::StatusOr<std::vector<ColumnLineage>> Relation(
      std::string_view name,
      int depth,
      std::vector<std::string_view>& hidden);
  // The columns of `name` as a query reads it: the innermost CTE of that name
  // in scope, or else the relation `name`.
  base::StatusOr<std::vector<ColumnLineage>>
  Read(std::string_view name, int depth, std::vector<std::string_view>& hidden);
  base::StatusOr<std::vector<ColumnLineage>> Select(SyntaqliteParser* p,
                                                    uint32_t id,
                                                    int depth);
  // Brings into scope the CTEs visible at `at`. Fails if `at` is not where a
  // query can be read, as the CTEs in scope there are not known.
  base::Status EnterScope(SqlNode at) {
    if (!Within(at.parser, syntaqlite_result_root(at.parser), at.id)) {
      return base::ErrStatus(
          "relation analysis: could not find the scope of the node being read");
    }
    // Defining the CTEs says nothing about the rows of what is read at `at`.
    preserves_rows_ = true;
    return base::OkStatus();
  }

  bool preserves_rows() const { return preserves_rows_; }

 private:
  struct ScopeRelation {
    std::string_view name;
    // No columns means the relation's shape is unknown.
    std::optional<std::vector<ColumnLineage>> columns;
    // Columns coalesced with a column to their left by USING or NATURAL JOIN.
    std::vector<std::string_view> hidden_from_star;
    // Hidden columns, left out of both `*` and `table.*`.
    std::vector<std::string_view> hidden;
  };
  using Scope = std::vector<ScopeRelation>;
  // A common table expression in scope. No columns means they could not be
  // worked out, as for a recursive one while it is still being defined.
  struct Cte {
    std::string_view name;
    std::optional<std::vector<ColumnLineage>> columns;
  };

  base::Status Sources(SyntaqliteParser* p,
                       uint32_t id,
                       int depth,
                       Scope* scope);
  base::StatusOr<std::vector<ColumnLineage>>
  SelectStmt(SyntaqliteParser* p, const SyntaqliteSelectStmt&, int depth);
  static ColumnLineage Lookup(const Scope&,
                              std::string_view table,
                              std::string_view column);
  // Brings every CTE of `with` into scope, of unknown shape, and returns the
  // index in `ctes_` of the first. As in SQLite, each CTE body sees all the
  // CTEs of its WITH clause, later ones and itself included, so the names
  // must all be in scope before any body is analyzed. One read before it is
  // worked out has unknown shape, so it fails rather than reading a relation
  // of the same name.
  size_t DeclareCtes(SyntaqliteParser* p, const SyntaqliteWithClause& with);
  // Works out the columns of the CTE declared at `ctes_[slot]`.
  void DefineCte(SyntaqliteParser* p,
                 const SyntaqliteCteDefinition& cte,
                 size_t slot,
                 int depth);
  // Brings into scope the CTEs defined on the way from `id` down to `at`,
  // through the places a query can be read: statements, WITH clauses,
  // compound selects, FROM clauses and pipeline sources. Returns whether `at`
  // was reached; if not, the CTEs in scope are unchanged.
  bool Within(SyntaqliteParser* p, uint32_t id, uint32_t at);
  bool WithinSources(SyntaqliteParser* p, uint32_t id, uint32_t at);
  // The innermost CTE in scope named `name`, if any.
  const Cte* FindCte(std::string_view name) const;
  // Renames `columns` to the names listed at `names`, if there is a list, as
  // `CREATE VIEW v(a, b)` and `WITH t(a, b)` do.
  static base::Status NameColumns(SyntaqliteParser* p,
                                  uint32_t names,
                                  std::string_view relation,
                                  std::vector<ColumnLineage>& columns);

  const Catalog& catalog_;
  // Lineage string_views point into each view's sql string and parse tree, so
  // every OwnedView needs a stable address: growing a std::vector<OwnedView>
  // would move the elements and moving `sql` can relocate its bytes (SSO).
  std::vector<std::unique_ptr<OwnedView>> views_;
  // Lineage string_views point into each leaf relation's strings, so they are
  // kept at stable addresses for the same reason.
  std::vector<std::unique_ptr<LeafRelation>> leaves_;
  // The CTEs in scope of what is being analyzed, innermost last.
  std::vector<Cte> ctes_;
  bool preserves_rows_ = true;
};

size_t RelationAnalyzer::Impl::DeclareCtes(SyntaqliteParser* p,
                                           const SyntaqliteWithClause& with) {
  size_t first = ctes_.size();
  for (const SyntaqliteCteDefinition* cte : CteDefinitions(p, with)) {
    ctes_.push_back({Text(p, cte->cte_name), std::nullopt});
  }
  return first;
}

void RelationAnalyzer::Impl::DefineCte(SyntaqliteParser* p,
                                       const SyntaqliteCteDefinition& cte,
                                       size_t slot,
                                       int depth) {
  base::StatusOr<std::vector<ColumnLineage>> columns =
      Select(p, cte.select, depth);
  if (columns.ok() &&
      NameColumns(p, cte.columns, ctes_[slot].name, *columns).ok()) {
    ctes_[slot].columns = std::move(*columns);
  }
}

bool RelationAnalyzer::Impl::Within(SyntaqliteParser* p,
                                    uint32_t id,
                                    uint32_t at) {
  if (id == at) {
    return true;
  }
  const SyntaqliteNode* node = Node(p, id);
  if (!node) {
    return false;
  }
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_CREATE_VIEW_STMT:
      return Within(p, node->create_view_stmt.select, at);
    case SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT:
      return Within(p, node->create_perfetto_view_stmt.select, at);
    case SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT:
      return Within(p, node->create_perfetto_table_stmt.select, at) ||
             Within(p, node->create_perfetto_table_stmt.pipeline, at);
    case SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT:
      return Within(p, node->create_perfetto_function_stmt.select, at);
    case SYNTAQLITE_NODE_CREATE_TABLE_STMT:
      return Within(p, node->create_table_stmt.as_select, at);
    case SYNTAQLITE_NODE_SELECT_STMT:
      return WithinSources(p, node->select_stmt.from_clause, at);
    case SYNTAQLITE_NODE_COMPOUND_SELECT:
      return Within(p, node->compound_select.left, at) ||
             Within(p, node->compound_select.right, at);
    case SYNTAQLITE_NODE_PERFETTO_PIPELINE:
      return Within(p, node->perfetto_pipeline.from, at) ||
             Within(p, node->perfetto_pipeline.intersection, at);
    case SYNTAQLITE_NODE_PERFETTO_INTERVAL_INTERSECTION: {
      const void* list = syntaqlite_parser_node(
          p, node->perfetto_interval_intersection.operands);
      uint32_t count = list ? syntaqlite_list_count(list) : 0;
      for (uint32_t i = 0; i < count; ++i) {
        if (Within(p, syntaqlite_list_child_id(list, i), at)) {
          return true;
        }
      }
      return false;
    }
    case SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE:
      return Within(p, node->perfetto_pipe_source.select, at);
    case SYNTAQLITE_NODE_WITH_CLAUSE: {
      const SyntaqliteWithClause& with = node->with_clause;
      size_t outer = DeclareCtes(p, with);
      size_t slot = outer;
      for (const SyntaqliteCteDefinition* cte : CteDefinitions(p, with)) {
        if (Within(p, cte->select, at)) {
          return true;
        }
        DefineCte(p, *cte, slot++, 0);
      }
      if (Within(p, with.select, at)) {
        return true;
      }
      ctes_.resize(outer);
      return false;
    }
    default:
      return false;
  }
}

bool RelationAnalyzer::Impl::WithinSources(SyntaqliteParser* p,
                                           uint32_t id,
                                           uint32_t at) {
  if (id == at) {
    return true;
  }
  const SyntaqliteNode* node = Node(p, id);
  if (!node) {
    return false;
  }
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_JOIN_CLAUSE:
      return WithinSources(p, node->join_clause.left, at) ||
             WithinSources(p, node->join_clause.right, at);
    case SYNTAQLITE_NODE_JOIN_PREFIX:
      return WithinSources(p, node->join_prefix.source, at);
    case SYNTAQLITE_NODE_SUBQUERY_TABLE_SOURCE:
      return Within(p, node->subquery_table_source.select, at);
    default:
      return false;
  }
}

base::StatusOr<std::vector<ColumnLineage>> RelationAnalyzer::Impl::Read(
    std::string_view name,
    int depth,
    std::vector<std::string_view>& hidden) {
  const Cte* cte = FindCte(name);
  if (!cte) {
    return Relation(name, depth, hidden);
  }
  // A CTE can filter or reorder its rows.
  preserves_rows_ = false;
  if (!cte->columns) {
    return base::ErrStatus("relation analysis: CTE '%.*s' has unknown shape",
                           static_cast<int>(name.size()), name.data());
  }
  return *cte->columns;
}

const RelationAnalyzer::Impl::Cte* RelationAnalyzer::Impl::FindCte(
    std::string_view name) const {
  for (auto it = ctes_.rbegin(); it != ctes_.rend(); ++it) {
    if (base::CaseInsensitiveEqual(it->name, name)) {
      return &*it;
    }
  }
  return nullptr;
}

base::Status RelationAnalyzer::Impl::NameColumns(
    SyntaqliteParser* p,
    uint32_t names,
    std::string_view relation,
    std::vector<ColumnLineage>& columns) {
  if (!syntaqlite_node_is_present(names)) {
    return base::OkStatus();
  }
  const void* list = syntaqlite_parser_node(p, names);
  uint32_t count = syntaqlite_list_count(list);
  if (count != columns.size()) {
    return base::ErrStatus(
        "relation analysis: '%.*s' names %u columns for %u results",
        static_cast<int>(relation.size()), relation.data(), count,
        static_cast<uint32_t>(columns.size()));
  }
  for (uint32_t i = 0; i < count; ++i) {
    const SyntaqliteNode* column = Node(p, syntaqlite_list_child_id(list, i));
    if (!column || column->tag != SYNTAQLITE_NODE_COLUMN_REF) {
      return base::ErrStatus("relation analysis: invalid column name in '%.*s'",
                             static_cast<int>(relation.size()),
                             relation.data());
    }
    columns[i].output_name = Text(p, column->column_ref.column);
  }
  return base::OkStatus();
}

ColumnLineage RelationAnalyzer::Impl::Lookup(const Scope& scope,
                                             std::string_view table,
                                             std::string_view column) {
  ColumnLineage found;
  found.output_name = column;
  for (const ScopeRelation& relation : scope) {
    if (!table.empty() && !base::CaseInsensitiveEqual(relation.name, table)) {
      continue;
    }
    if (!relation.columns) {
      // A relation of unknown shape might be where this column came from.
      return found;
    }
    for (const ColumnLineage& candidate : *relation.columns) {
      if (!base::CaseInsensitiveEqual(candidate.output_name, column)) {
        continue;
      }
      AppendUniqueOrigins(&found.origins, candidate.origins);
    }
  }
  return found;
}

base::Status RelationAnalyzer::Impl::Sources(SyntaqliteParser* p,
                                             uint32_t id,
                                             int depth,
                                             Scope* scope) {
  const SyntaqliteNode* node = Node(p, id);
  if (!node) {
    return base::OkStatus();
  }
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_JOIN_CLAUSE: {
      size_t begin = scope->size();
      RETURN_IF_ERROR(Sources(p, node->join_clause.left, depth, scope));
      size_t right = scope->size();
      RETURN_IF_ERROR(Sources(p, node->join_clause.right, depth, scope));

      std::vector<std::string_view> joined;
      if (syntaqlite_node_is_present(node->join_clause.using_columns)) {
        const void* list =
            syntaqlite_parser_node(p, node->join_clause.using_columns);
        uint32_t count = syntaqlite_list_count(list);
        for (uint32_t i = 0; i < count; ++i) {
          const SyntaqliteNode* column =
              Node(p, syntaqlite_list_child_id(list, i));
          if (column && column->tag == SYNTAQLITE_NODE_COLUMN_REF) {
            joined.push_back(Text(p, column->column_ref.column));
          }
        }
      } else if (IsNatural(node->join_clause.join_type)) {
        std::vector<std::string_view> left_names;
        for (size_t i = begin; i < right; ++i) {
          if (!(*scope)[i].columns) {
            continue;
          }
          for (const ColumnLineage& column : *(*scope)[i].columns) {
            if (!ContainsName((*scope)[i].hidden_from_star,
                              column.output_name)) {
              left_names.push_back(column.output_name);
            }
          }
        }
        for (size_t i = right; i < scope->size(); ++i) {
          if (!(*scope)[i].columns) {
            continue;
          }
          for (const ColumnLineage& column : *(*scope)[i].columns) {
            if (ContainsName(left_names, column.output_name) &&
                !ContainsName(joined, column.output_name)) {
              joined.push_back(column.output_name);
            }
          }
        }
      }
      for (size_t i = right; i < scope->size(); ++i) {
        if (!(*scope)[i].columns) {
          continue;
        }
        for (const ColumnLineage& column : *(*scope)[i].columns) {
          if (ContainsName(joined, column.output_name) &&
              !ContainsName((*scope)[i].hidden_from_star, column.output_name)) {
            (*scope)[i].hidden_from_star.push_back(column.output_name);
          }
        }
      }
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_JOIN_PREFIX:
      return Sources(p, node->join_prefix.source, depth, scope);
    case SYNTAQLITE_NODE_TABLE_REF: {
      std::string_view name = Text(p, node->table_ref.table_name);
      std::string_view alias = name;
      if (const SyntaqliteNode* a = Node(p, node->table_ref.alias)) {
        alias = Text(p, a->ident_name.source);
      }
      // A CTE hides any relation of the same name. It is never qualified by
      // a schema.
      std::vector<std::string_view> hidden;
      base::StatusOr<std::vector<ColumnLineage>> columns =
          Text(p, node->table_ref.schema).empty()
              ? Read(name, depth, hidden)
              : Relation(name, depth, hidden);
      if (!columns.ok()) {
        scope->push_back({alias, std::nullopt, {}, {}});
        return base::OkStatus();
      }
      scope->push_back({alias, std::move(*columns), {}, std::move(hidden)});
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_SUBQUERY_TABLE_SOURCE: {
      std::string_view alias;
      if (const SyntaqliteNode* a =
              Node(p, node->subquery_table_source.alias)) {
        alias = Text(p, a->ident_name.source);
      }
      base::StatusOr<std::vector<ColumnLineage>> columns =
          Select(p, node->subquery_table_source.select, depth);
      if (!columns.ok()) {
        scope->push_back({alias, std::nullopt, {}, {}});
        return base::OkStatus();
      }
      scope->push_back({alias, std::move(*columns), {}, {}});
      return base::OkStatus();
    }
    default:
      preserves_rows_ = false;
      scope->push_back({{}, std::nullopt, {}, {}});
      return base::OkStatus();
  }
}

base::StatusOr<std::vector<ColumnLineage>> RelationAnalyzer::Impl::SelectStmt(
    SyntaqliteParser* p,
    const SyntaqliteSelectStmt& select,
    int depth) {
  Scope scope;
  RETURN_IF_ERROR(Sources(p, select.from_clause, depth, &scope));

  if (syntaqlite_node_is_present(select.where_clause) ||
      syntaqlite_node_is_present(select.groupby) ||
      syntaqlite_node_is_present(select.having) ||
      syntaqlite_node_is_present(select.orderby) ||
      syntaqlite_node_is_present(select.limit_clause) ||
      select.flags.bits.distinct || scope.size() != 1) {
    preserves_rows_ = false;
  }

  const auto* list = static_cast<const SyntaqliteResultColumnList*>(
      syntaqlite_parser_node(p, select.columns));
  if (!list) {
    return base::ErrStatus("relation analysis: a select with no columns");
  }

  std::vector<ColumnLineage> out;
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; ++i) {
    const SyntaqliteNode* item = Node(p, syntaqlite_list_child_id(list, i));
    if (!item) {
      continue;
    }
    const SyntaqliteResultColumn& column = item->result_column;
    if (column.flags.bits.star) {
      std::string_view table;
      if (const SyntaqliteNode* e = Node(p, column.expr)) {
        if (e->tag == SYNTAQLITE_NODE_COLUMN_REF) {
          table = Text(p, e->column_ref.table);
        } else if (e->tag == SYNTAQLITE_NODE_IDENT_NAME) {
          table = Text(p, e->ident_name.source);
        }
      }
      for (const ScopeRelation& relation : scope) {
        if (!table.empty() &&
            !base::CaseInsensitiveEqual(relation.name, table)) {
          continue;
        }
        if (!relation.columns) {
          return base::ErrStatus(
              "relation analysis: '*' over a relation of unknown shape");
        }
        for (const ColumnLineage& c : *relation.columns) {
          if (ContainsName(relation.hidden, c.output_name) ||
              (table.empty() &&
               ContainsName(relation.hidden_from_star, c.output_name))) {
            continue;
          }
          out.push_back(c);
        }
      }
      continue;
    }

    std::string_view alias;
    if (const SyntaqliteNode* a = Node(p, column.alias)) {
      alias = Text(p, a->ident_name.source);
    }
    const SyntaqliteNode* expr = Node(p, column.expr);
    if (expr && expr->tag == SYNTAQLITE_NODE_COLUMN_REF) {
      ColumnLineage resolved = Lookup(scope, Text(p, expr->column_ref.table),
                                      Text(p, expr->column_ref.column));
      if (!alias.empty()) {
        resolved.output_name = alias;
      }
      out.push_back(std::move(resolved));
      continue;
    }
    preserves_rows_ = false;
    out.emplace_back(ColumnLineage{alias, {}});
  }
  return out;
}

base::StatusOr<std::vector<ColumnLineage>>
RelationAnalyzer::Impl::Select(SyntaqliteParser* p, uint32_t id, int depth) {
  const SyntaqliteNode* node = Node(p, id);
  if (!node) {
    return base::ErrStatus("relation analysis: nothing to read from");
  }
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_SELECT_STMT:
      return SelectStmt(p, node->select_stmt, depth);
    case SYNTAQLITE_NODE_WITH_CLAUSE: {
      preserves_rows_ = false;
      size_t outer = DeclareCtes(p, node->with_clause);
      size_t slot = outer;
      for (const SyntaqliteCteDefinition* cte :
           CteDefinitions(p, node->with_clause)) {
        DefineCte(p, *cte, slot++, depth);
      }
      base::StatusOr<std::vector<ColumnLineage>> columns =
          Select(p, node->with_clause.select, depth);
      ctes_.resize(outer);
      return columns;
    }
    case SYNTAQLITE_NODE_COMPOUND_SELECT: {
      preserves_rows_ = false;
      base::StatusOr<std::vector<ColumnLineage>> left =
          Select(p, node->compound_select.left, depth);
      RETURN_IF_ERROR(left.status());
      base::StatusOr<std::vector<ColumnLineage>> right =
          Select(p, node->compound_select.right, depth);
      RETURN_IF_ERROR(right.status());
      if (left->size() != right->size()) {
        return base::ErrStatus("relation analysis: arms of differing widths");
      }
      for (uint32_t i = 0; i < left->size(); ++i) {
        // An arm with no origins contributes rows that cannot be traced, so
        // keeping only the other arm's origins would claim more than we know.
        if ((*left)[i].origins.empty() || (*right)[i].origins.empty()) {
          (*left)[i].origins.clear();
        } else {
          AppendUniqueOrigins(&(*left)[i].origins, (*right)[i].origins);
        }
      }
      return std::move(*left);
    }
    default:
      return base::ErrStatus("relation analysis: not a select");
  }
}

base::StatusOr<std::vector<ColumnLineage>> RelationAnalyzer::Impl::Relation(
    std::string_view name,
    int depth,
    std::vector<std::string_view>& hidden) {
  if (std::optional<LeafRelation> found = catalog_.FindLeafRelation(name)) {
    leaves_.push_back(std::make_unique<LeafRelation>(std::move(*found)));
    const LeafRelation* relation = leaves_.back().get();
    std::vector<ColumnLineage> out;
    out.reserve(relation->columns.size());
    for (const LeafColumn& column : relation->columns) {
      out.push_back(
          {column.name, {{relation->name, column.name, column.type}}});
      if (column.hidden) {
        hidden.push_back(column.name);
      }
    }
    return out;
  }
  if (depth >= kMaxDepth) {
    return base::ErrStatus(
        "relation analysis: views nested too deeply at '%.*s'",
        static_cast<int>(name.size()), name.data());
  }
  std::optional<std::string> sql = catalog_.FindViewSql(name);
  if (!sql) {
    return base::ErrStatus("relation analysis: '%.*s' is not known",
                           static_cast<int>(name.size()), name.data());
  }

  // TODO(lalitm): Cache parsed view definitions instead of allocating a parser
  // for each resolved view; the query-time cost is acceptable for now.
  auto view = std::make_unique<OwnedView>();
  view->sql = std::move(*sql);
  view->parser.reset(syntaqlite_parser_create_perfetto(nullptr));
  syntaqlite_parser_reset(view->parser.get(), view->sql.data(),
                          static_cast<uint32_t>(view->sql.size()));
  if (syntaqlite_parser_next(view->parser.get()) != SYNTAQLITE_PARSE_OK) {
    return base::ErrStatus("relation analysis: could not parse view '%.*s'",
                           static_cast<int>(name.size()), name.data());
  }
  view->root = syntaqlite_result_root(view->parser.get());
  OwnedView* owned = view.get();
  views_.push_back(std::move(view));

  SyntaqliteParser* p = owned->parser.get();
  const SyntaqliteNode* node = Node(p, owned->root);
  if (!node) {
    return base::ErrStatus("relation analysis: empty view '%.*s'",
                           static_cast<int>(name.size()), name.data());
  }
  uint32_t select = 0;
  uint32_t column_names = 0;
  if (node->tag == SYNTAQLITE_NODE_CREATE_VIEW_STMT) {
    select = node->create_view_stmt.select;
    column_names = node->create_view_stmt.column_names;
  } else if (node->tag == SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT) {
    select = node->create_perfetto_view_stmt.select;
  } else {
    return base::ErrStatus("relation analysis: '%.*s' is not a view",
                           static_cast<int>(name.size()), name.data());
  }
  // A view sees none of the CTEs of the query reading it.
  std::vector<Cte> outer = std::exchange(ctes_, {});
  base::StatusOr<std::vector<ColumnLineage>> columns =
      Select(p, select, depth + 1);
  ctes_ = std::move(outer);
  RETURN_IF_ERROR(columns.status());
  RETURN_IF_ERROR(NameColumns(p, column_names, name, *columns));
  return {columns};
}

Catalog::~Catalog() = default;

std::optional<ColumnType> ColumnLineage::type() const {
  std::optional<ColumnType> result;
  for (const ColumnOrigin& origin : origins) {
    if (!origin.type || (result && !(*result == *origin.type))) {
      return std::nullopt;
    }
    result = origin.type;
  }
  return result;
}

RelationLineage::RelationLineage(std::unique_ptr<Storage> storage)
    : storage_(std::move(storage)) {}

RelationLineage::RelationLineage(RelationLineage&&) noexcept = default;
RelationLineage& RelationLineage::operator=(RelationLineage&&) noexcept =
    default;
RelationLineage::~RelationLineage() = default;

const std::vector<ColumnLineage>& RelationLineage::columns() const {
  return storage_->columns();
}

std::optional<std::string_view> RelationLineage::row_origin() const {
  return storage_->row_origin();
}

RelationAnalyzer::RelationAnalyzer(const Catalog& catalog)
    : impl_(std::make_unique<Impl>(catalog)) {}

RelationAnalyzer::~RelationAnalyzer() = default;

base::StatusOr<RelationLineage> RelationAnalyzer::AnalyzeQuery(SqlNode query) {
  impl_->Begin();
  RETURN_IF_ERROR(impl_->EnterScope(query));
  ASSIGN_OR_RETURN(auto columns, impl_->Select(query.parser, query.id, 0));
  return RelationLineage(std::make_unique<RelationLineage::Storage>(
      std::move(columns), impl_->preserves_rows()));
}

base::StatusOr<RelationLineage> RelationAnalyzer::AnalyzeRelation(
    SqlNode at,
    std::string_view name) {
  impl_->Begin();
  RETURN_IF_ERROR(impl_->EnterScope(at));
  // Hidden columns are still columns of the relation itself.
  std::vector<std::string_view> hidden;
  ASSIGN_OR_RETURN(auto columns, impl_->Read(name, 0, hidden));
  return RelationLineage(std::make_unique<RelationLineage::Storage>(
      std::move(columns), impl_->preserves_rows()));
}

}  // namespace perfetto::perfetto_sql::analysis
