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

#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/variant.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor::pipeline {

namespace analysis = ::perfetto::perfetto_sql::analysis;
using core::StorageType;

const OperationRegistration Scan::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE, &BuildPlan};

base::Status Scan::BuildPlan(Compiler* c, uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), from);
  ASSIGN_OR_RETURN(SourceRelation relation, Scan::BuildRelation(c, from));
  for (NamedColumn& column : relation.columns) {
    c->Append(std::move(column), from);
  }
  if (std::optional<std::string> qualifier = Scan::SourceQualifier(c, *n)) {
    c->AddAlias({std::move(*qualifier), c->row()});
  }
  return base::OkStatus();
}

base::StatusOr<SourceRelation> Scan::BuildRelation(Compiler* c,
                                                   uint32_t source) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), source);
  Scan scan;
  if (const dataframe::Dataframe* dataframe =
          Scan::FindDirectDataframe(c, *n)) {
    scan = Scan::BuildDataframeScan(c, *dataframe,
                                    SpanText(c->parser(), n->table_name));
  } else {
    ASSIGN_OR_RETURN(scan, Scan::BuildSqlScan(c, source));
  }
  RETURN_IF_ERROR(Scan::CheckSourceNames(c, scan.columns_, source));
  SourceRelation relation;
  relation.columns = scan.columns_;
  relation.node = c->AddNode(std::move(scan));
  return relation;
}

const dataframe::Dataframe* Scan::FindDirectDataframe(
    Compiler* c,
    const SyntaqlitePerfettoPipeSource& n) {
  // Only an unqualified table name is read directly; anything else goes to
  // SQLite. An alias only renames the qualifier, so it does not matter here.
  bool table_name =
      !syntaqlite_node_is_present(n.select) && !IsSpanPresent(n.schema);
  if (!table_name) {
    return nullptr;
  }
  const dataframe::Dataframe* dataframe =
      c->catalog().FindDataframe(SpanText(c->parser(), n.table_name));
  // Columns can only be shared out of a dataframe which has stopped changing.
  return dataframe && dataframe->finalized() ? dataframe : nullptr;
}

std::optional<std::string> Scan::SourceQualifier(
    Compiler* c,
    const SyntaqlitePerfettoPipeSource& n) {
  // As in SQLite, an alias replaces the table name.
  if (syntaqlite_node_is_present(n.alias)) {
    const auto* alias = Node<SyntaqliteName>(c->parser(), n.alias);
    return SpanText(c->parser(), alias->ident_name.source);
  }
  if (!syntaqlite_node_is_present(n.select)) {
    return SpanText(c->parser(), n.table_name);
  }
  return std::nullopt;
}

base::Status Scan::CheckSourceNames(Compiler* c,
                                    const std::vector<NamedColumn>& columns,
                                    uint32_t at) {
  for (size_t i = 0; i < columns.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (base::CaseInsensitiveEqual(columns[j].name, columns[i].name)) {
        return c->Expected(at,
                           "distinct column names, but there are two named '" +
                               columns[j].name + "'");
      }
    }
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string& name = columns[i].name;
    // An expression is only named by its alias.
    if (name.empty()) {
      return c->Expected(at, "every column to have a name, but column " +
                                 std::to_string(i + 1) +
                                 " has none: give it one with AS");
    }
    if (!sql_argument::IsValidColumnName(base::StringView(name))) {
      return c->Expected(at, "every column to have a valid name, but '" + name +
                                 "' is not one: give it one with AS");
    }
  }
  return base::OkStatus();
}

Scan Scan::BuildDataframeScan(Compiler* c,
                              const dataframe::Dataframe& dataframe,
                              std::string name) {
  Scan scan;
  Scan::Dataframe source;
  source.name = std::move(name);
  source.row_count = dataframe.row_count();
  const std::vector<std::string>& names = dataframe.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    // Hidden from SELECT * in SQLite, so hidden here too.
    if (dataframe::IsHiddenColumn(names[i])) {
      continue;
    }
    Scan::AddScanColumn(c, &scan, {names[i], dataframe.column_type(i)});
    source.columns.push_back(dataframe.shared_column(i));
  }
  scan.source_ = std::move(source);
  return scan;
}

base::StatusOr<Scan> Scan::BuildSqlScan(Compiler* c, uint32_t from) {
  // The columns come from semantic analysis alone: the SQL is only run where
  // the pipeline is written, which is the only place everything it reads is
  // in scope.
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), from);
  if (IsSpanPresent(n->schema)) {
    return c->Err(from, Compiler::Error::kUnsupported,
                  "reading a relation whose columns cannot be worked out",
                  " (a schema-qualified table)");
  }
  analysis::RelationAnalyzer analyzer(c->catalog());
  base::StatusOr<analysis::RelationLineage> lineage =
      syntaqlite_node_is_present(n->select)
          ? analyzer.AnalyzeQuery({c->parser(), n->select})
          : analyzer.AnalyzeRelation({c->parser(), from},
                                     SpanText(c->parser(), n->table_name));
  if (!lineage.ok()) {
    return c->Err(from, Compiler::Error::kUnsupported,
                  "reading a relation whose columns cannot be worked out",
                  " (" + lineage.status().message() + ")");
  }
  Scan scan;
  // The relation as written, which the SQL building its dataframe reads.
  scan.source_ = c->SourceForNode(from);
  for (const analysis::ColumnLineage& column : lineage->columns()) {
    std::optional<StorageType> type;
    if (std::optional<analysis::ColumnType> traced = column.type()) {
      type = sql_schema::ToStorageType(*traced);
      // An Id only means something in the table it indexes: read through
      // SQL, it is a plain number.
      if (type->Is<core::Id>()) {
        type = StorageType{core::Uint32{}};
      }
    }
    Scan::AddScanColumn(c, &scan, {std::string(column.output_name), type});
  }
  return scan;
}

void Scan::AddScanColumn(Compiler* c, Scan* scan, ColumnSchema column) {
  ColumnId id = c->AddColumn(column.name, column.type);
  scan->columns_.push_back({std::move(column.name), id});
}

}  // namespace perfetto::trace_processor::pipeline
