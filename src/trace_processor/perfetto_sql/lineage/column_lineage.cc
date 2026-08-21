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

#include "src/trace_processor/perfetto_sql/lineage/column_lineage.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/utils.h"

namespace perfetto::trace_processor::lineage {
namespace {

using core::Double;
using core::Int64;
using core::StorageType;

// How far a chain of views is traced before giving up. A view can only refer
// to what already existed, so only something pathological reaches this.
constexpr int kMaxDepth = 32;

struct ParserDeleter {
  void operator()(SyntaqliteParser* p) const { syntaqlite_parser_destroy(p); }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, ParserDeleter>;

std::string Text(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  return std::string(base::TrimWhitespace(SyntaqliteSpanText(p, span)));
}

// A relation in scope. No columns means its shape is unknown, as for a plain
// SQLite table, which is not the same as a relation whose columns are known
// but untyped.
struct Relation {
  std::string name;
  std::optional<std::vector<ResolvedColumn>> columns;
};

// The type two sources of one column agree on, which is the type a column
// joined with USING has.
std::optional<StorageType> Weaken(const std::optional<StorageType>& a,
                                  const std::optional<StorageType>& b) {
  if (!a || !b) {
    return std::nullopt;
  }
  if (*a == *b) {
    return a;
  }
  bool numeric = (a->Is<Int64>() || a->Is<Double>()) &&
                 (b->Is<Int64>() || b->Is<Double>());
  return numeric ? std::make_optional(StorageType{Double{}}) : std::nullopt;
}

class Resolver {
 public:
  explicit Resolver(const Catalog& catalog) : catalog_(catalog) {}

  base::StatusOr<std::vector<ResolvedColumn>> Relation(const std::string& name,
                                                       int depth);
  base::StatusOr<std::vector<ResolvedColumn>> Select(SyntaqliteParser* p,
                                                     uint32_t id,
                                                     int depth);

  // Set while resolving when nothing between the rows and the dataframe
  // modified them.
  bool passthrough() const { return passthrough_; }

 private:
  using Scope = std::vector<lineage::Relation>;

  base::Status Sources(SyntaqliteParser* p,
                       uint32_t id,
                       int depth,
                       Scope* scope);
  base::StatusOr<std::vector<ResolvedColumn>>
  SelectStmt(SyntaqliteParser* p, const SyntaqliteSelectStmt&, int depth);
  ResolvedColumn Lookup(const Scope&,
                        const std::string& table,
                        const std::string& column) const;

  const Catalog& catalog_;
  bool passthrough_ = true;
};

const SyntaqliteNode* Node(SyntaqliteParser* p, uint32_t id) {
  if (!syntaqlite_node_is_present(id)) {
    return nullptr;
  }
  return static_cast<const SyntaqliteNode*>(syntaqlite_parser_node(p, id));
}

ResolvedColumn Resolver::Lookup(const Scope& scope,
                                const std::string& table,
                                const std::string& column) const {
  ResolvedColumn found;
  found.name = column;
  bool any = false;
  for (const lineage::Relation& relation : scope) {
    if (!table.empty() && !base::CaseInsensitiveEqual(relation.name, table)) {
      continue;
    }
    if (!relation.columns) {
      // A relation of unknown shape might be where this column came from.
      return found;
    }
    for (const ResolvedColumn& candidate : *relation.columns) {
      if (!base::CaseInsensitiveEqual(candidate.name, column)) {
        continue;
      }
      // Named in more than one relation, so this is a USING or NATURAL join:
      // the column is whatever both sides agree on and has no single source.
      if (any) {
        found.type = Weaken(found.type, candidate.type);
        found.dataframe.clear();
        found.dataframe_column.clear();
      } else {
        found.type = candidate.type;
        found.dataframe = candidate.dataframe;
        found.dataframe_column = candidate.dataframe_column;
      }
      any = true;
    }
  }
  return found;
}

base::Status Resolver::Sources(SyntaqliteParser* p,
                               uint32_t id,
                               int depth,
                               Scope* scope) {
  const SyntaqliteNode* node = Node(p, id);
  if (!node) {
    return base::OkStatus();
  }
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_JOIN_CLAUSE:
      RETURN_IF_ERROR(Sources(p, node->join_clause.left, depth, scope));
      return Sources(p, node->join_clause.right, depth, scope);
    case SYNTAQLITE_NODE_JOIN_PREFIX:
      return Sources(p, node->join_prefix.source, depth, scope);
    case SYNTAQLITE_NODE_TABLE_REF: {
      std::string name = Text(p, node->table_ref.table_name);
      std::string alias = name;
      if (const SyntaqliteNode* a = Node(p, node->table_ref.alias)) {
        alias = Text(p, a->ident_name.source);
      }
      base::StatusOr<std::vector<ResolvedColumn>> columns =
          Relation(name, depth);
      if (!columns.ok()) {
        scope->push_back({std::move(alias), std::nullopt});
        return base::OkStatus();
      }
      scope->push_back({std::move(alias), std::move(*columns)});
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_SUBQUERY_TABLE_SOURCE: {
      std::string alias;
      if (const SyntaqliteNode* a =
              Node(p, node->subquery_table_source.alias)) {
        alias = Text(p, a->ident_name.source);
      }
      base::StatusOr<std::vector<ResolvedColumn>> columns =
          Select(p, node->subquery_table_source.select, depth);
      if (!columns.ok()) {
        scope->push_back({std::move(alias), std::nullopt});
        return base::OkStatus();
      }
      scope->push_back({std::move(alias), std::move(*columns)});
      return base::OkStatus();
    }
    default:
      // Anything else in a FROM clause is a shape this does not understand.
      passthrough_ = false;
      scope->push_back({std::string(), std::nullopt});
      return base::OkStatus();
  }
}

base::StatusOr<std::vector<ResolvedColumn>> Resolver::SelectStmt(
    SyntaqliteParser* p,
    const SyntaqliteSelectStmt& select,
    int depth) {
  Scope scope;
  RETURN_IF_ERROR(Sources(p, select.from_clause, depth, &scope));

  // Anything which is not a plain projection of a single source means the rows
  // are no longer the dataframe's own.
  if (syntaqlite_node_is_present(select.where_clause) ||
      syntaqlite_node_is_present(select.groupby) ||
      syntaqlite_node_is_present(select.having) ||
      syntaqlite_node_is_present(select.limit_clause) ||
      select.flags.bits.distinct || scope.size() != 1) {
    passthrough_ = false;
  }

  const auto* list = static_cast<const SyntaqliteResultColumnList*>(
      syntaqlite_parser_node(p, select.columns));
  if (!list) {
    return base::ErrStatus("column lineage: a select with no columns");
  }

  std::vector<ResolvedColumn> out;
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; ++i) {
    const SyntaqliteNode* item = Node(p, syntaqlite_list_child_id(list, i));
    if (!item) {
      continue;
    }
    const SyntaqliteResultColumn& column = item->result_column;
    if (column.flags.bits.star) {
      // `*` or `t.*`: every column of the relations it covers, in order.
      std::string table;
      if (const SyntaqliteNode* e = Node(p, column.expr)) {
        if (e->tag == SYNTAQLITE_NODE_COLUMN_REF) {
          table = Text(p, e->column_ref.table);
        }
      }
      for (const lineage::Relation& relation : scope) {
        if (!table.empty() &&
            !base::CaseInsensitiveEqual(relation.name, table)) {
          continue;
        }
        if (!relation.columns) {
          return base::ErrStatus(
              "column lineage: '*' over a relation of unknown shape");
        }
        for (const ResolvedColumn& c : *relation.columns) {
          out.push_back(c);
        }
      }
      continue;
    }

    std::string alias;
    if (const SyntaqliteNode* a = Node(p, column.alias)) {
      alias = Text(p, a->ident_name.source);
    }
    const SyntaqliteNode* expr = Node(p, column.expr);
    if (expr && expr->tag == SYNTAQLITE_NODE_COLUMN_REF) {
      ResolvedColumn resolved = Lookup(scope, Text(p, expr->column_ref.table),
                                       Text(p, expr->column_ref.column));
      if (!alias.empty()) {
        resolved.name = alias;
      }
      out.push_back(std::move(resolved));
      continue;
    }
    // Anything which is not a column reference cannot be traced back.
    passthrough_ = false;
    ResolvedColumn unfollowed;
    unfollowed.name = std::move(alias);
    out.push_back(std::move(unfollowed));
  }
  return out;
}

base::StatusOr<std::vector<ResolvedColumn>>
Resolver::Select(SyntaqliteParser* p, uint32_t id, int depth) {
  const SyntaqliteNode* node = Node(p, id);
  if (!node) {
    return base::ErrStatus("column lineage: nothing to read from");
  }
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_SELECT_STMT:
      return SelectStmt(p, node->select_stmt, depth);
    case SYNTAQLITE_NODE_WITH_CLAUSE:
      passthrough_ = false;
      // CTEs are only reachable by name, and an unknown name is a relation of
      // unknown shape, which is already handled.
      return Select(p, node->with_clause.select, depth);
    case SYNTAQLITE_NODE_COMPOUND_SELECT: {
      passthrough_ = false;
      base::StatusOr<std::vector<ResolvedColumn>> left =
          Select(p, node->compound_select.left, depth);
      RETURN_IF_ERROR(left.status());
      base::StatusOr<std::vector<ResolvedColumn>> right =
          Select(p, node->compound_select.right, depth);
      RETURN_IF_ERROR(right.status());
      if (left->size() != right->size()) {
        return base::ErrStatus("column lineage: arms of differing widths");
      }
      for (uint32_t i = 0; i < left->size(); ++i) {
        ResolvedColumn& column = (*left)[i];
        const ResolvedColumn& other = (*right)[i];
        column.type = Weaken(column.type, other.type);
        if (column.dataframe != other.dataframe ||
            column.dataframe_column != other.dataframe_column) {
          column.dataframe.clear();
          column.dataframe_column.clear();
        }
      }
      return *left;
    }
    default:
      return base::ErrStatus("column lineage: not a select");
  }
}

base::StatusOr<std::vector<ResolvedColumn>> Resolver::Relation(
    const std::string& name,
    int depth) {
  if (const std::vector<ResolvedColumn>* columns = catalog_.Dataframe(name)) {
    // The end of the trail: a dataframe's columns are their own source.
    std::vector<ResolvedColumn> out = *columns;
    for (ResolvedColumn& column : out) {
      column.dataframe = name;
      column.dataframe_column = column.name;
    }
    return out;
  }
  std::optional<std::string> view = catalog_.ViewSql(name);
  if (!view) {
    return base::ErrStatus("column lineage: '%s' is not a dataframe or a view",
                           name.c_str());
  }
  if (depth >= kMaxDepth) {
    return base::ErrStatus("column lineage: views nested too deeply at '%s'",
                           name.c_str());
  }
  // Parsed rather than pattern matched: SQLite stores the whole CREATE VIEW
  // statement, and its body is read exactly as a subquery would be.
  ScopedParser p(syntaqlite_parser_create_perfetto(nullptr));
  syntaqlite_parser_reset(p.get(), view->data(),
                          static_cast<uint32_t>(view->size()));
  if (syntaqlite_parser_next(p.get()) != SYNTAQLITE_PARSE_OK) {
    return base::ErrStatus("column lineage: could not parse view '%s'",
                           name.c_str());
  }
  const SyntaqliteNode* node = Node(p.get(), syntaqlite_result_root(p.get()));
  if (!node) {
    return base::ErrStatus("column lineage: empty view '%s'", name.c_str());
  }
  uint32_t select = 0;
  if (node->tag == SYNTAQLITE_NODE_CREATE_VIEW_STMT) {
    select = node->create_view_stmt.select;
  } else if (node->tag == SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT) {
    select = node->create_perfetto_view_stmt.select;
  } else {
    return base::ErrStatus("column lineage: '%s' is not a view", name.c_str());
  }
  return Select(p.get(), select, depth + 1);
}

}  // namespace

Catalog::~Catalog() = default;

std::string SoleDataframe(const std::vector<ResolvedColumn>& columns) {
  if (columns.empty()) {
    return std::string();
  }
  const std::string& first = columns.front().dataframe;
  if (first.empty()) {
    return std::string();
  }
  for (const ResolvedColumn& column : columns) {
    if (column.dataframe != first) {
      return std::string();
    }
  }
  return first;
}

base::StatusOr<std::vector<ResolvedColumn>> ResolveSelect(
    const std::string& sql,
    const Catalog& catalog) {
  ScopedParser p(syntaqlite_parser_create_perfetto(nullptr));
  syntaqlite_parser_reset(p.get(), sql.data(),
                          static_cast<uint32_t>(sql.size()));
  if (syntaqlite_parser_next(p.get()) != SYNTAQLITE_PARSE_OK) {
    return base::ErrStatus("column lineage: could not parse");
  }
  Resolver resolver(catalog);
  return resolver.Select(p.get(), syntaqlite_result_root(p.get()), 0);
}

base::StatusOr<std::vector<ResolvedColumn>> ResolveRelation(
    const std::string& name,
    const Catalog& catalog) {
  Resolver resolver(catalog);
  return resolver.Relation(name, 0);
}

std::string PassthroughDataframe(const std::string& name,
                                 const Catalog& catalog) {
  Resolver resolver(catalog);
  base::StatusOr<std::vector<ResolvedColumn>> columns =
      resolver.Relation(name, 0);
  if (!columns.ok() || !resolver.passthrough()) {
    return std::string();
  }
  return SoleDataframe(*columns);
}

}  // namespace perfetto::trace_processor::lineage
