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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor::pipesql {
namespace {

namespace analysis = ::perfetto::perfetto_sql::analysis;
using core::StorageType;
using pipeline::Builder;
using pipeline::NamedColumn;
using pipeline::NameId;
using pipeline::PlanType;

bool IsSubpipeline(Translator* t, const SyntaqlitePerfettoPipeSource& n) {
  if (!syntaqlite_node_is_present(n.select)) {
    return false;
  }
  const auto* node = Node<SyntaqliteNode>(t->parser(), n.select);
  return node->tag == SYNTAQLITE_NODE_PERFETTO_PIPELINE;
}

const dataframe::Dataframe* FindDirectDataframe(
    Translator* t,
    const SyntaqlitePerfettoPipeSource& n) {
  bool table_name =
      !syntaqlite_node_is_present(n.select) && !IsSpanPresent(n.schema);
  if (!table_name) {
    return nullptr;
  }
  const dataframe::Dataframe* dataframe =
      t->catalog().FindDataframe(SpanText(t->parser(), n.table_name));
  // Columns can only be shared out of a finalized dataframe.
  return dataframe && dataframe->finalized() ? dataframe : nullptr;
}

base::StatusOr<analysis::RelationLineage> AnalyzeSource(Translator* t,
                                                        uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(t->parser(), from);
  if (IsSpanPresent(n->schema)) {
    return t->Err(from, Translator::Error::kUnsupported,
                  "reading a relation whose columns cannot be worked out",
                  " (a schema-qualified table)");
  }
  analysis::RelationAnalyzer analyzer(t->catalog());
  base::StatusOr<analysis::RelationLineage> lineage =
      syntaqlite_node_is_present(n->select)
          ? analyzer.AnalyzeQuery({t->parser(), n->select})
          : analyzer.AnalyzeRelation({t->parser(), from},
                                     SpanText(t->parser(), n->table_name));
  if (!lineage.ok()) {
    return t->Err(from, Translator::Error::kUnsupported,
                  "reading a relation whose columns cannot be worked out",
                  " (" + lineage.status().message() + ")");
  }
  return lineage;
}

const dataframe::Dataframe* FindRenamedDataframe(
    Translator* t,
    const analysis::RelationLineage& lineage,
    std::vector<Builder::TableColumn>* columns) {
  std::optional<std::string_view> origin = lineage.row_origin();
  if (!origin) {
    return nullptr;
  }
  const dataframe::Dataframe* dataframe = t->catalog().FindDataframe(*origin);
  if (!dataframe || !dataframe->finalized()) {
    return nullptr;
  }
  for (const analysis::ColumnLineage& column : lineage.columns()) {
    if (column.origins.size() != 1) {
      return nullptr;
    }
    std::optional<uint32_t> i = pipeline::FindNonHiddenColumn(
        *dataframe, column.origins[0].column_name);
    if (!i) {
      return nullptr;
    }
    columns->push_back({t->Name(column.output_name), *i});
  }
  return dataframe;
}

base::Status CheckSourceNames(Translator* t,
                              const std::vector<NamedColumn>& columns,
                              uint32_t at) {
  for (size_t i = 0; i < columns.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (t->Same(columns[j].name, columns[i].name)) {
        return t->Expected(at,
                           "distinct column names, but there are two named '" +
                               t->Text(columns[j].name) + "'");
      }
    }
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string& name = t->Text(columns[i].name);
    if (name.empty()) {
      return t->Expected(at, "every column to have a name, but column " +
                                 std::to_string(i + 1) +
                                 " has none: give it one with AS");
    }
    if (!sql_argument::IsValidColumnName(base::StringView(name))) {
      return t->Expected(at, "every column to have a valid name, but '" + name +
                                 "' is not one: give it one with AS");
    }
  }
  return base::OkStatus();
}

}  // namespace

std::optional<NameId> SourceQualifier(Translator* t,
                                      const SyntaqlitePerfettoPipeSource& n) {
  if (syntaqlite_node_is_present(n.alias)) {
    const auto* alias = Node<SyntaqliteName>(t->parser(), n.alias);
    return t->Name(alias->ident_name.source);
  }
  if (!syntaqlite_node_is_present(n.select)) {
    return t->Name(n.table_name);
  }
  return std::nullopt;
}

base::StatusOr<Builder::ScanResult> TranslateSourceRelation(Translator* t,
                                                            uint32_t source) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(t->parser(), source);
  if (IsSubpipeline(t, *n)) {
    ASSIGN_OR_RETURN(Builder::ScanResult sub,
                     t->TranslateSubpipeline(n->select));
    RETURN_IF_ERROR(CheckSourceNames(t, sub.columns, source));
    return sub;
  }
  Builder::ScanResult out;
  if (const dataframe::Dataframe* dataframe = FindDirectDataframe(t, *n)) {
    // Hidden from SELECT * in SQLite, so hidden here too.
    std::vector<Builder::TableColumn> columns;
    const std::vector<std::string>& names = dataframe->column_names();
    for (uint32_t i = 0; i < names.size(); ++i) {
      if (!dataframe::IsHiddenColumn(names[i])) {
        columns.push_back({t->Name(names[i]), i});
      }
    }
    out = t->builder().ScanTable(*dataframe,
                                 SpanText(t->parser(), n->table_name), columns);
  } else {
    ASSIGN_OR_RETURN(analysis::RelationLineage lineage,
                     AnalyzeSource(t, source));
    std::vector<Builder::TableColumn> columns;
    if (const dataframe::Dataframe* renamed =
            FindRenamedDataframe(t, lineage, &columns)) {
      out = t->builder().ScanTable(*renamed, *lineage.row_origin(), columns);
    } else {
      std::vector<Builder::SqlColumn> sql_cols;
      sql_cols.reserve(lineage.columns().size());
      for (const analysis::ColumnLineage& col : lineage.columns()) {
        PlanType type = PlanType::Unknown();
        if (std::optional<analysis::ColumnType> traced = col.type()) {
          StorageType storage = sql_schema::ToStorageType(*traced);
          // An Id only means something in the table it indexes: read through
          // SQL, it is a plain number.
          if (storage.Is<core::Id>()) {
            storage = core::Uint32{};
          }
          type = storage;
        }
        sql_cols.push_back({t->Name(col.output_name), type});
      }
      out = t->builder().ScanSql(t->SourceForNode(source).sql(), sql_cols);
    }
  }
  RETURN_IF_ERROR(CheckSourceNames(t, out.columns, source));
  return out;
}

base::Status TranslateFrom(Translator* t, uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(t->parser(), from);
  ASSIGN_OR_RETURN(Builder::ScanResult relation,
                   TranslateSourceRelation(t, from));
  t->SetRoot(relation.relation);
  for (NamedColumn& column : relation.columns) {
    t->Append(column, from);
  }
  if (std::optional<NameId> qualifier = SourceQualifier(t, *n)) {
    t->AddAlias(*qualifier, 0);
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipesql
