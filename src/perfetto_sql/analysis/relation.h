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
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/type_set.h"

struct SyntaqliteParser;

namespace perfetto::perfetto_sql::analysis {

// A node in a caller-owned Syntaqlite parse tree.
struct SqlNode {
  SyntaqliteParser* parser;
  uint32_t id;
};

// How a leaf relation column is stored, as declared by the catalog. Mirrors
// the storage vocabulary of the engine behind the catalog without depending
// on it.
struct Id {};
struct Uint32 {};
struct Int32 {};
struct Int64 {};
struct Double {};
struct String {};
using ColumnType = base::TypeSet<Id, Uint32, Int32, Int64, Double, String>;

// A leaf relation whose columns can be used as lineage origins.
struct LeafColumn {
  std::string_view name;
  // Nothing when the catalog does not know how the column is stored.
  std::optional<ColumnType> type;
};
struct LeafRelation {
  std::string_view name;
  std::vector<LeafColumn> columns;
};

// Supplies the schema objects referenced by parsed queries. Returned leaf
// strings only need to remain valid for the duration of an Analyze call.
class Catalog {
 public:
  virtual ~Catalog();

  virtual std::optional<LeafRelation> FindLeafRelation(
      std::string_view name) const = 0;
  virtual std::optional<std::string> FindViewSql(
      std::string_view name) const = 0;
};

struct ColumnOrigin {
  std::string_view relation_name;
  std::string_view column_name;
  std::optional<ColumnType> type;

  // Two origins naming the same column always store it the same way, so the
  // type does not participate.
  bool operator==(const ColumnOrigin& other) const {
    return relation_name == other.relation_name &&
           column_name == other.column_name;
  }
};

struct ColumnLineage {
  std::string_view output_name;
  // Empty when the expression cannot be traced to a known leaf column. USING
  // columns and compound queries can have more than one origin.
  std::vector<ColumnOrigin> origins;

  // The type every origin agrees on, or nothing when the column has no
  // origins, an origin is untyped, or the origins disagree.
  std::optional<ColumnType> type() const;
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
  std::optional<std::string_view> row_origin() const;

 private:
  class Storage;

  explicit RelationLineage(std::unique_ptr<Storage>);

  std::unique_ptr<Storage> storage_;

  friend class RelationAnalyzer;
};

// Computes relation and column lineage over caller-owned parse trees.
class RelationAnalyzer {
 public:
  explicit RelationAnalyzer(const Catalog&);
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
