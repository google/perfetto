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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_COLUMN_LINEAGE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_COLUMN_LINEAGE_H_

#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::lineage {

// One output column of the relation a pipeline reads from.
struct ResolvedColumn {
  std::string name;
  // Nothing when the column could not be traced back to a dataframe, which
  // covers every expression and everything read from a plain SQLite table.
  std::optional<core::StorageType> type;
  // The dataframe column this was traced back to, empty when it was not. A
  // view which only renames the columns it reads is a re-export, so tracing
  // sees through any number of such views.
  std::string dataframe;
  std::string dataframe_column;
};

// The single dataframe every column of `columns` was traced back to, or empty
// when they came from more than one dataframe or from none.
std::string SoleDataframe(const std::vector<ResolvedColumn>& columns);

// Resolves the names which can appear in a FROM clause.
class Catalog {
 public:
  virtual ~Catalog();

  // The columns of the dataframe registered as `name`, or null when no
  // dataframe is registered under that name.
  virtual const std::vector<ResolvedColumn>* Dataframe(
      const std::string& name) const = 0;

  // The CREATE VIEW statement registered as `name` exactly as SQLite stored
  // it, or nothing when `name` is not a view.
  virtual std::optional<std::string> ViewSql(const std::string& name) const = 0;
};

// The columns produced by `sql`, which is what a pipeline reads from: either
// a SELECT or a bare relation name.
//
// A column has a type only when it can be traced back to a dataframe, and is
// unknown otherwise; nothing is guessed. Views are traced through by parsing
// the SQL SQLite stored for them, as if the body had been written out in
// place.
base::StatusOr<std::vector<ResolvedColumn>> ResolveSelect(
    const std::string& sql,
    const Catalog&);
base::StatusOr<std::vector<ResolvedColumn>> ResolveRelation(
    const std::string& name,
    const Catalog&);

// The dataframe `name` reads straight through to, or empty when it changes
// the rows in any way.
//
// This is stronger than SoleDataframe and the difference matters: every column
// of `SELECT id FROM slice WHERE dur > 5` comes from slice, but the relation
// itself is not slice. Only a relation which filters nothing, joins nothing,
// groups nothing and computes nothing can be read as the dataframe itself.
std::string PassthroughDataframe(const std::string& name, const Catalog&);

}  // namespace perfetto::trace_processor::lineage

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_COLUMN_LINEAGE_H_
