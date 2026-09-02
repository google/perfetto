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

std::string_view Text(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  return base::TrimWhitespace(SyntaqliteSpanText(p, span));
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
  }

  base::StatusOr<std::vector<ColumnLineage>> Relation(std::string_view name,
                                                      int depth);
  base::StatusOr<std::vector<ColumnLineage>> Select(SyntaqliteParser* p,
                                                    uint32_t id,
                                                    int depth);

  bool preserves_rows() const { return preserves_rows_; }

 private:
  struct ScopeRelation {
    std::string_view name;
    // No columns means the relation's shape is unknown.
    std::optional<std::vector<ColumnLineage>> columns;
    // Columns coalesced with a column to their left by USING or NATURAL JOIN.
    std::vector<std::string_view> hidden_from_star;
  };
  using Scope = std::vector<ScopeRelation>;

  base::Status Sources(SyntaqliteParser* p,
                       uint32_t id,
                       int depth,
                       Scope* scope);
  base::StatusOr<std::vector<ColumnLineage>>
  SelectStmt(SyntaqliteParser* p, const SyntaqliteSelectStmt&, int depth);
  static ColumnLineage Lookup(const Scope&,
                              std::string_view table,
                              std::string_view column);

  const Catalog& catalog_;
  // Lineage string_views point into each view's sql string and parse tree, so
  // every OwnedView needs a stable address: growing a std::vector<OwnedView>
  // would move the elements and moving `sql` can relocate its bytes (SSO).
  std::vector<std::unique_ptr<OwnedView>> views_;
  bool preserves_rows_ = true;
};

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
      base::StatusOr<std::vector<ColumnLineage>> columns =
          Relation(name, depth);
      if (!columns.ok()) {
        scope->push_back({alias, std::nullopt, {}});
        return base::OkStatus();
      }
      scope->push_back({alias, std::move(*columns), {}});
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
        scope->push_back({alias, std::nullopt, {}});
        return base::OkStatus();
      }
      scope->push_back({alias, std::move(*columns), {}});
      return base::OkStatus();
    }
    default:
      preserves_rows_ = false;
      scope->push_back({{}, std::nullopt, {}});
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
          if (table.empty() &&
              ContainsName(relation.hidden_from_star, c.output_name)) {
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
    case SYNTAQLITE_NODE_WITH_CLAUSE:
      preserves_rows_ = false;
      return Select(p, node->with_clause.select, depth);
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
    int depth) {
  if (std::optional<LeafRelation> relation = catalog_.FindLeafRelation(name)) {
    std::vector<ColumnLineage> out;
    out.reserve(relation->columns.size());
    for (std::string_view column : relation->columns) {
      out.push_back({column, {{relation->name, column}}});
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
  base::StatusOr<std::vector<ColumnLineage>> columns =
      Select(p, select, depth + 1);
  RETURN_IF_ERROR(columns.status());
  if (syntaqlite_node_is_present(column_names)) {
    const void* list = syntaqlite_parser_node(p, column_names);
    uint32_t count = syntaqlite_list_count(list);
    if (count != columns->size()) {
      return base::ErrStatus(
          "relation analysis: view '%.*s' names %u columns for %u results",
          static_cast<int>(name.size()), name.data(), count,
          static_cast<uint32_t>(columns->size()));
    }
    for (uint32_t i = 0; i < count; ++i) {
      const SyntaqliteNode* column = Node(p, syntaqlite_list_child_id(list, i));
      if (!column || column->tag != SYNTAQLITE_NODE_COLUMN_REF) {
        return base::ErrStatus(
            "relation analysis: invalid column name in '%.*s'",
            static_cast<int>(name.size()), name.data());
      }
      (*columns)[i].output_name = Text(p, column->column_ref.column);
    }
  }
  return {columns};
}

Catalog::~Catalog() = default;

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
  ASSIGN_OR_RETURN(auto columns, impl_->Select(query.parser, query.id, 0));
  return RelationLineage(std::make_unique<RelationLineage::Storage>(
      std::move(columns), impl_->preserves_rows()));
}

base::StatusOr<RelationLineage> RelationAnalyzer::AnalyzeRelation(
    std::string_view name) {
  impl_->Begin();
  ASSIGN_OR_RETURN(auto columns, impl_->Relation(name, 0));
  return RelationLineage(std::make_unique<RelationLineage::Storage>(
      std::move(columns), impl_->preserves_rows()));
}

}  // namespace perfetto::perfetto_sql::analysis
