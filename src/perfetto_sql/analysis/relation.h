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

#ifndef SRC_PERFETTO_SQL_ANALYSIS_RELATION_H_
#define SRC_PERFETTO_SQL_ANALYSIS_RELATION_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/program.h"

struct SyntaqliteParser;

namespace perfetto::perfetto_sql::analysis {

// A node in a caller-owned Syntaqlite parse tree.
struct SqlNode {
  SyntaqliteParser* parser;
  uint32_t id;
};

// A leaf relation whose columns can be used as lineage origins.
struct LeafRelation {
  std::vector<std::string_view> columns;
};

// A view definition already parsed by the caller.
struct ViewDefinition {
  SqlNode statement;
};

// Supplies the schema objects referenced by parsed queries. Returned strings
// and parse trees only need to remain valid for the duration of an Analyze
// call.
class Catalog {
 public:
  virtual ~Catalog();

  virtual std::optional<LeafRelation> FindLeafRelation(
      std::string_view name) const = 0;
  virtual std::optional<ViewDefinition> FindView(
      std::string_view name) const = 0;
};

struct ColumnOrigin {
  SymbolId relation;
  std::string_view column_name;

  bool operator==(const ColumnOrigin& other) const {
    return relation == other.relation && column_name == other.column_name;
  }
};

struct ColumnLineage {
  std::string_view output_name;
  // Empty when the expression cannot be traced to a known leaf column. USING
  // columns and compound queries can have more than one origin.
  std::vector<ColumnOrigin> origins;
};

// The lineage of a query result considered as a relation. All strings are
// owned by this object.
class RelationLineage {
 public:
  RelationLineage(RelationLineage&&) noexcept;
  RelationLineage& operator=(RelationLineage&&) noexcept;
  ~RelationLineage();

  RelationLineage(const RelationLineage&) = delete;
  RelationLineage& operator=(const RelationLineage&) = delete;

  const std::vector<ColumnLineage>& columns() const;

  // The leaf relation whose rows map directly to the output rows, or nothing
  // when the query filters, joins, groups, limits or computes values.
  std::optional<SymbolId> row_origin() const;

 private:
  class Storage;

  explicit RelationLineage(std::unique_ptr<Storage>);

  std::unique_ptr<Storage> storage_;

  friend class RelationAnalyzer;
};

// Computes relation and column lineage over caller-owned parse trees.
class RelationAnalyzer {
 public:
  RelationAnalyzer(const Program&, const Catalog&);
  ~RelationAnalyzer();

  RelationAnalyzer(const RelationAnalyzer&) = delete;
  RelationAnalyzer& operator=(const RelationAnalyzer&) = delete;

  base::StatusOr<RelationLineage> AnalyzeQuery(SqlNode);
  base::StatusOr<RelationLineage> AnalyzeRelation(std::string_view name);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace perfetto::perfetto_sql::analysis

#endif  // SRC_PERFETTO_SQL_ANALYSIS_RELATION_H_
