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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_STAGES_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_STAGES_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"
#include "src/trace_processor/perfetto_sql/pipesql/translator.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipesql {

// Unquotes `span` as written in SQL (doubled closing quotes unescaped).
std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span);

// True if `span` was present in source (including an empty quoted name `""`).
bool IsSpanPresent(SyntaqliteTextSpan span);

template <typename T>
const T* Node(SyntaqliteParser* p, uint32_t id) {
  return static_cast<const T*>(syntaqlite_parser_node(p, id));
}

// Translates a PipeSQL AST into a pipeline::PreparedStatement via
// pipeline::Builder, maintaining the visible column scope and table aliases.
class Translator {
 public:
  struct RowColumn {
    pipeline::NamedColumn column;
    const char* operation;
    uint32_t node;
    // Reachable only through a table alias (e.g. intersection operand ts/dur).
    bool qualified_only = false;
  };

  struct Alias {
    pipeline::NameId name;
    std::vector<RowColumn> columns;
  };

  enum class Error {
    kExpected,
    kUnsupported,
    kNoSuchColumn,
    kAmbiguousColumn,
    kNoSuchAlias,
  };

  struct TranslatedAggregates {
    std::vector<pipeline::Builder::AggregateItem> items;
    // AST node ID for each item, for scope diagnostics.
    std::vector<uint32_t> nodes;
  };

  Translator(SyntaqliteParser* p, const NodeSourceFn& source, const Catalog& c);

  base::Status TranslatePipeline(uint32_t pipeline);
  pipeline::PreparedStatement Finish();
  base::StatusOr<pipeline::Builder::ScanResult> TranslateSubpipeline(
      uint32_t pipeline);

  pipeline::Builder& builder() { return builder_; }
  const pipeline::Builder& builder() const { return builder_; }
  pipeline::Builder::Relation root() const { return root_; }
  void SetRoot(pipeline::Builder::Relation root) { root_ = root; }

  pipeline::NameId Name(std::string_view text) {
    return builder_.InternName(text);
  }
  pipeline::NameId Name(SyntaqliteTextSpan span) {
    return Name(SpanText(p_, span));
  }
  const std::string& Text(pipeline::NameId name) const {
    return builder_.NameText(name);
  }
  bool Same(pipeline::NameId a, pipeline::NameId b) const {
    return builder_.SameName(a, b);
  }
  pipeline::PlanType ColumnType(pipeline::ColumnId col) const {
    return builder_.ColumnType(col);
  }

  std::optional<pipeline::ColumnId> FindColumn(
      const std::vector<pipeline::NamedColumn>& columns,
      pipeline::NameId name) const;
  const Alias* FindAlias(pipeline::NameId name) const;
  base::StatusOr<size_t> FindInRow(pipeline::NameId name, uint32_t at) const;
  base::Status NoSuchColumn(pipeline::NameId name, uint32_t at) const;
  base::Status CheckListedOnce(std::vector<pipeline::NameId>& seen,
                               pipeline::NameId name,
                               uint32_t at) const;

  base::StatusOr<TranslatedAggregates> TranslateAggregateList(uint32_t list_id);
  base::StatusOr<pipeline::ColumnId> ResolveColumnExpr(uint32_t expr);
  base::StatusOr<pipeline::ColumnId> Resolve(pipeline::NameId name,
                                             uint32_t at) const;
  base::StatusOr<pipeline::ColumnId> Resolve(
      std::optional<pipeline::NameId> qualifier,
      pipeline::NameId name,
      uint32_t at) const;

  SqlSource SourceForNode(uint32_t node) const;
  base::Status Err(uint32_t at,
                   Error,
                   std::string_view what,
                   const std::string& detail = "") const;
  base::Status Expected(uint32_t at, std::string_view what) const;
  base::Status Unsupported(uint32_t at, std::string_view what) const;
  std::string AmbiguousCandidates(
      const std::vector<const RowColumn*>& matches) const;

  void SetOperation(const char* operation);
  void ReplaceRow(std::vector<RowColumn> row);
  void ExtendRow(std::vector<RowColumn> columns);
  void DropColumn(pipeline::NameId name);
  void RenameColumn(size_t at, pipeline::NameId name, uint32_t node);
  void SetColumnValue(size_t at, pipeline::ColumnId value, uint32_t node);
  void Append(pipeline::NamedColumn column,
              uint32_t node,
              bool qualified_only = false);

  void ClearAliases();
  void AddAlias(Alias alias);
  // Makes the row's columns from `first_column` onwards reachable as
  // `name.column`.
  void AddAlias(pipeline::NameId name, size_t first_column);
  void ReplaceAliasColumns(
      const std::vector<std::pair<pipeline::ColumnId, pipeline::ColumnId>>&
          replaced);
  void RemoveAlias(pipeline::NameId name);

  SyntaqliteParser* parser() const { return p_; }
  const Catalog& catalog() const { return catalog_; }
  const std::vector<RowColumn>& row() const { return scope_.row; }
  const char* operation() const { return scope_.operation; }

 private:
  struct Scope {
    const char* operation = "FROM";
    std::vector<RowColumn> row;
    std::vector<Alias> aliases;
  };

  base::Status TranslateStage(uint32_t stage);
  std::string Origin(const RowColumn& column) const;
  std::vector<pipeline::NamedColumn> VisibleColumns() const;

  SyntaqliteParser* p_;
  const NodeSourceFn& source_;
  const Catalog& catalog_;

  pipeline::Builder builder_;
  pipeline::Builder::Relation root_;
  Scope scope_;
};

// Source / FROM stage (`scan_stage.cc`).
base::Status TranslateFrom(Translator*, uint32_t from);
base::StatusOr<pipeline::Builder::ScanResult> TranslateSourceRelation(
    Translator*,
    uint32_t source);
std::optional<pipeline::NameId> SourceQualifier(
    Translator*,
    const SyntaqlitePerfettoPipeSource& n);

// Projection stages (`projection_stages.cc`).
base::Status TranslateSelect(Translator*, uint32_t stage);
base::Status TranslateExtend(Translator*, uint32_t stage);
base::Status TranslateDrop(Translator*, uint32_t stage);
base::Status TranslateRename(Translator*, uint32_t stage);
base::Status TranslateSet(Translator*, uint32_t stage);
base::Status TranslateAs(Translator*, uint32_t stage);

// Relational stages (`relational_stages.cc`).
base::Status TranslateWhere(Translator*, uint32_t stage);
base::Status TranslateAggregate(Translator*, uint32_t stage);
base::Status TranslateDistinct(Translator*, uint32_t stage);
base::Status TranslateJoin(Translator*, uint32_t stage);
base::Status TranslateOrderBy(Translator*, uint32_t stage);
base::Status TranslateLimit(Translator*, uint32_t stage);

// Tree and graph stages (`tree_stages.cc`).
base::Status TranslateTreeAccumulate(Translator*, uint32_t stage);
base::Status TranslateTreeKeyPaths(Translator*, uint32_t stage);
base::Status TranslateGraphTree(Translator*, uint32_t node);

// Interval stages (`interval_stages.cc`).
base::Status TranslateIntervalIntersect(Translator*, uint32_t node);
base::Status TranslateIntervalFlatten(Translator*, uint32_t stage);
base::Status TranslateIntervalFillGaps(Translator*, uint32_t stage);

}  // namespace perfetto::trace_processor::pipesql

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_STAGES_H_
