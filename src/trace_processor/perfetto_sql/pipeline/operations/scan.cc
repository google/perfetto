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
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

namespace analysis = ::perfetto::perfetto_sql::analysis;
using core::StorageType;

namespace {

std::optional<uint32_t> FindScanColumn(const dataframe::Dataframe& dataframe,
                                       std::string_view name) {
  const std::vector<std::string>& names = dataframe.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    if (names[i] == name && !dataframe::IsHiddenColumn(names[i])) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

const OperationRegistration Scan::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE, &BuildPlan,
    OperationRegistration::Encoding{0, true, &DecodePlan}};

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
    // Hidden from SELECT * in SQLite, so hidden here too.
    std::vector<DataframeColumn> columns;
    const std::vector<std::string>& names = dataframe->column_names();
    for (uint32_t i = 0; i < names.size(); ++i) {
      if (!dataframe::IsHiddenColumn(names[i])) {
        columns.push_back({names[i], i});
      }
    }
    scan = Scan::BuildDataframeScan(
        c, *dataframe, SpanText(c->parser(), n->table_name), columns);
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
                              std::string name,
                              const std::vector<DataframeColumn>& columns) {
  Scan scan;
  Scan::Dataframe source;
  source.name = std::move(name);
  source.row_count = dataframe.row_count();
  for (const DataframeColumn& column : columns) {
    Scan::AddScanColumn(c, &scan,
                        {column.name, dataframe.column_type(column.index)});
    source.column_names.push_back(dataframe.column_names()[column.index]);
    source.columns.push_back(dataframe.shared_column(column.index));
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
  // A relation which keeps a dataframe's rows, only choosing and renaming its
  // columns, is read from the dataframe rather than through SQLite. Its
  // columns are then all plain references to the dataframe's.
  if (std::optional<std::string_view> origin = lineage->row_origin()) {
    const dataframe::Dataframe* dataframe = c->catalog().FindDataframe(*origin);
    std::vector<DataframeColumn> columns;
    // As for a table read directly, the dataframe must have stopped changing.
    for (const analysis::ColumnLineage& column : lineage->columns()) {
      std::optional<uint32_t> i;
      if (dataframe && dataframe->finalized() && column.origins.size() == 1) {
        i = FindScanColumn(*dataframe, column.origins[0].column_name);
      }
      if (!i) {
        break;
      }
      columns.push_back({std::string(column.output_name), *i});
    }
    if (dataframe && columns.size() == lineage->columns().size()) {
      return Scan::BuildDataframeScan(c, *dataframe, std::string(*origin),
                                      columns);
    }
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

std::optional<uint32_t> Scan::Prune(std::vector<bool>* needed) {
  auto& scan = *this;
  std::vector<uint32_t> keep;
  for (uint32_t i = 0; i < scan.columns_.size(); ++i) {
    if ((*needed)[scan.columns_[i].id]) {
      keep.push_back(i);
    }
  }
  if (keep.size() == scan.columns_.size()) {
    return std::nullopt;
  }
  // Always keep at least one column: a batch with no columns has no rows.
  if (keep.empty()) {
    keep.push_back(0);
  }
  std::vector<NamedColumn> kept_columns;
  for (uint32_t i : keep) {
    kept_columns.push_back(std::move(scan.columns_[i]));
  }
  scan.columns_ = std::move(kept_columns);

  switch (scan.source_.index()) {
    case base::variant_index<Scan::Source, Scan::Dataframe>(): {
      auto& dataframe = base::unchecked_get<Scan::Dataframe>(scan.source_);
      std::vector<std::string> kept_names;
      std::vector<std::shared_ptr<const dataframe::Column>> kept;
      for (uint32_t i : keep) {
        kept_names.push_back(std::move(dataframe.column_names[i]));
        kept.push_back(std::move(dataframe.columns[i]));
      }
      dataframe.column_names = std::move(kept_names);
      dataframe.columns = std::move(kept);
      return std::nullopt;
    }
    case base::variant_index<Scan::Source, SqlSource>():
      // The SQL building a dataframe of the relation reads only the columns
      // the scan keeps.
      return std::nullopt;
    default:
      PERFETTO_FATAL("Unknown scan source");
  }
}

void Scan::Lower(Lowering* c, const PlanNode&) const {
  const auto& scan = *this;

  PERFETTO_DCHECK(!c->has_source());
  for (const NamedColumn& column : scan.columns_) {
    c->Define(column.id);
  }
  c->SetSource(scan.MakeSource());
  // A dataframe is read in order, so a column it keeps sorted ascends.
  const auto& dataframe = base::unchecked_get<Scan::Dataframe>(scan.source_);
  for (uint32_t i = 0; i < scan.columns_.size(); ++i) {
    if (!dataframe.columns[i]->sort_state.Is<core::Unsorted>()) {
      c->AddAscendingColumn(scan.columns_[i].id);
    }
  }
}

std::unique_ptr<ex::Source> Scan::MakeSource() const {
  const auto& scan = *this;
  switch (scan.source_.index()) {
    case base::variant_index<Scan::Source, Scan::Dataframe>(): {
      const auto& data = base::unchecked_get<Scan::Dataframe>(scan.source_);
      return std::make_unique<ex::DataframeScan>(data.columns, data.row_count);
    }
    default:
      // SQL is moved out into dataframe arguments, and those are bound to
      // dataframes, before a plan is run.
      PERFETTO_FATAL("Unknown scan source");
  }
}

const OperationRegistration& Scan::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Scan::Clone() const {
  return std::make_unique<Scan>(*this);
}

void Scan::Write(PlanWriter* c,
                 const PlanNode&,
                 Available* available_columns) const {
  auto& available = *available_columns;
  const auto& scan = *this;
  c->writer().U8(static_cast<uint8_t>(scan.source_.index()));
  switch (scan.source_.index()) {
    case base::variant_index<Scan::Source, Scan::Dataframe>():
      c->writer().Str(base::unchecked_get<Scan::Dataframe>(scan.source_).name);
      break;
    case base::variant_index<Scan::Source, Scan::DataframeArg>():
      c->writer().U32(
          base::unchecked_get<Scan::DataframeArg>(scan.source_).index);
      break;
    default:
      // SQL is moved out into dataframe arguments before a plan is written.
      PERFETTO_FATAL("Unknown scan source");
  }
  const auto* data = std::get_if<Scan::Dataframe>(&scan.source_);
  c->writer().Size(scan.columns_.size());
  available.clear();
  for (size_t i = 0; i < scan.columns_.size(); ++i) {
    const NamedColumn& column = scan.columns_[i];
    c->writer().Str(column.name);
    WriteType(&c->writer(), c->plan().columns()[column.id].type);
    // The dataframe column read, if the scan renames it.
    if (data) {
      const std::string& read = data->column_names[i];
      c->writer().Str(read == column.name ? std::string_view()
                                          : std::string_view(read));
    }
    available.push_back(column.id);
  }
}

void Scan::DecodePlan(PlanReader* c, Available* available_columns) {
  auto& available = *available_columns;
  Scan scan;
  available = ReadPayload(c, &scan);
  c->AddNode(std::move(scan));
}

Available Scan::ReadPayload(PlanReader* c, Scan* scan) {
  switch (c->reader().U8()) {
    case base::variant_index<Scan::Source, Scan::Dataframe>(): {
      Scan::Dataframe source;
      source.name = c->reader().Str();
      scan->source_ = std::move(source);
      break;
    }
    case base::variant_index<Scan::Source, Scan::DataframeArg>():
      scan->source_ = Scan::DataframeArg{c->reader().U32()};
      break;
    default:
      c->reader().Fail();
      return {};
  }
  auto* data = std::get_if<Scan::Dataframe>(&scan->source_);
  scan->columns_.resize(c->reader().Count());
  Available available;
  for (NamedColumn& column : scan->columns_) {
    column.name = c->reader().Str();
    column.id = c->AddColumn(column.name, ReadType(&c->reader()));
    if (data) {
      std::string read = c->reader().Str();
      data->column_names.push_back(read.empty() ? column.name
                                                : std::move(read));
    }
    available.push_back(column.id);
  }
  return available;
}

base::Status Scan::ResolveDataframes(LogicalPlan* logical_plan,
                                     const Catalog& catalog) {
  auto& plan = *logical_plan;
  auto& scan = *this;
  auto* data = std::get_if<Scan::Dataframe>(&scan.source_);
  if (!data) {
    return base::OkStatus();
  }
  const dataframe::Dataframe* dataframe = catalog.FindDataframe(data->name);
  if (!dataframe) {
    return base::ErrStatus("Pipeline: table '%s' no longer exists",
                           data->name.c_str());
  }
  for (size_t j = 0; j < scan.columns_.size(); ++j) {
    std::optional<uint32_t> i =
        FindScanColumn(*dataframe, data->column_names[j]);
    const std::optional<core::StorageType>& type =
        plan.columns()[scan.columns_[j].id].type;
    if (!i || !type || !(*type == dataframe->column_type(*i))) {
      return base::ErrStatus(
          "Pipeline: table '%s' has changed since the pipeline was written",
          data->name.c_str());
    }
    data->columns.push_back(dataframe->shared_column(*i));
  }
  data->row_count = dataframe->row_count();

  return base::OkStatus();
}

base::Status Scan::BindDataframeArgs(
    LogicalPlan* logical_plan,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool) {
  auto& plan = *logical_plan;
  auto& scan = *this;
  const auto* arg = std::get_if<Scan::DataframeArg>(&scan.source_);
  if (!arg) {
    return base::OkStatus();
  }
  if (arg->index >= args.size()) {
    return base::ErrStatus("Pipeline: no dataframe argument %u", arg->index);
  }
  // A relation with no rows is passed as null: read it as empty columns.
  std::optional<dataframe::Dataframe> empty;
  const dataframe::Dataframe* dataframe = args[arg->index];
  if (!dataframe) {
    std::vector<std::string> names;
    for (const NamedColumn& column : scan.columns_) {
      names.push_back(column.name);
    }
    dataframe::AdhocDataframeBuilder::Options options;
    options.emit_auto_id = false;
    ASSIGN_OR_RETURN(
        empty, dataframe::AdhocDataframeBuilder(std::move(names), pool, options)
                   .Build());
    dataframe = &*empty;
  }
  Scan::Dataframe data;
  data.name = "dataframe argument " + std::to_string(arg->index);
  for (const NamedColumn& column : scan.columns_) {
    std::optional<uint32_t> i = FindScanColumn(*dataframe, column.name);
    if (!i) {
      return base::ErrStatus("Pipeline: %s has no column '%s'",
                             data.name.c_str(), column.name.c_str());
    }
    // The dataframe was built after the plan was written, so it decides
    // what each column holds.
    plan.columns()[column.id].type = dataframe->column_type(*i);
    data.column_names.push_back(column.name);
    data.columns.push_back(dataframe->shared_column(*i));
  }
  data.row_count = dataframe->row_count();
  scan.source_ = std::move(data);

  return base::OkStatus();
}

void Scan::MoveSqlSourcesToDataframeArgs(std::vector<std::string>* sql_args) {
  auto& args = *sql_args;
  auto& scan = *this;
  if (!std::holds_alternative<SqlSource>(scan.source_)) {
    return;
  }
  std::vector<std::string> names;
  std::vector<std::string> references;
  for (const NamedColumn& column : scan.columns_) {
    names.push_back(column.name);
    references.push_back(QuoteIdentifier(column.name));
  }
  const std::string& from = base::unchecked_get<SqlSource>(scan.source_).sql();
  args.push_back("SELECT " + std::string(kDataframeAggFunction) + "(" +
                 QuoteString(base::Join(names, ",")) + ", " +
                 base::Join(references, ", ") + ") FROM " + from);
  scan.source_ = Scan::DataframeArg{static_cast<uint32_t>(args.size() - 1)};
}

}  // namespace perfetto::trace_processor::pipeline
