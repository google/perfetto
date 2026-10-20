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
#include "src/trace_processor/core/common/duplicate_types.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/row_estimate.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/eq_scans.h"
#include "src/trace_processor/core/exec/index_scan.h"
#include "src/trace_processor/core/exec/sorted_scan.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"
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

bool HoldsNoValues(const dataframe::Column& column, uint32_t rows) {
  if (rows == 0) {
    return true;
  }
  const core::BitVector* present = column.null_storage.MaybeGetNullBitVector();
  return present && !present->AnyBitSet();
}

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

// The indexes of `dataframe`, naming their columns as it does.
std::vector<ScanIndex> IndexesOf(const dataframe::Dataframe& dataframe) {
  std::vector<ScanIndex> indexes;
  for (const dataframe::Index& index : dataframe.indexes()) {
    ScanIndex scan_index;
    for (uint32_t column : index.columns()) {
      scan_index.column_names.push_back(dataframe.column_names()[column]);
    }
    scan_index.permutation = index.permutation_vector();
    indexes.push_back(std::move(scan_index));
  }
  return indexes;
}

// How a WHERE's predicates on a scan of a dataframe are searched for, as the
// dataframe's planner would search: through the columns it keeps sorted, or
// else through an index. Either way, the rest are tested on the rows found.
struct Searches {
  enum class Kind : uint8_t { kNone, kSorted, kIndex };
  Kind kind = Kind::kNone;
  // The predicates searched for: for an index, in the order of its columns.
  std::vector<uint32_t> searched;
  // By predicate, the scan column it tests, if any.
  std::vector<std::optional<uint32_t>> positions;
  // By predicate, how a sorted column is searched for it, if it can be.
  std::vector<std::optional<dataframe::logical::FilterStrategy>> strategies;
  // The index searched, for kIndex.
  uint32_t index = 0;
};

Searches PlanSearches(const ScanDataframe& data,
                      const std::vector<NamedColumn>& columns,
                      const std::vector<WherePredicate>& predicates) {
  Searches out;
  out.positions.resize(predicates.size());
  out.strategies.resize(predicates.size());
  bool sorted_eq = false;
  bool sorted_range = false;
  for (uint32_t p = 0; p < predicates.size(); ++p) {
    for (uint32_t i = 0; i < columns.size() && i < data.columns.size(); ++i) {
      if (columns[i].id == predicates[p].column) {
        out.positions[p] = i;
      }
    }
    if (!out.positions[p]) {
      continue;
    }
    out.strategies[p] = dataframe::SortedFilterStrategy(
        *data.columns[*out.positions[p]], predicates[p].op);
    if (out.strategies[p]) {
      (predicates[p].op.Is<core::Eq>() ? sorted_eq : sorted_range) = true;
    }
  }

  // An index serves equalities on its leading columns, as many as it can.
  // An Id column is always sorted, so never needs one.
  std::vector<uint32_t> best;
  uint32_t best_index = 0;
  for (uint32_t x = 0; x < data.indexes.size() && data.row_count > 1; ++x) {
    std::vector<uint32_t> matched;
    for (const std::string& name : data.indexes[x].column_names) {
      std::optional<uint32_t> found;
      for (uint32_t p = 0; p < predicates.size() && !found; ++p) {
        std::optional<uint32_t> position = out.positions[p];
        if (position && predicates[p].op.Is<core::Eq>() &&
            data.column_names[*position] == name &&
            !data.columns[*position]->storage.type().Is<core::Id>() &&
            std::find(matched.begin(), matched.end(), p) == matched.end()) {
          found = p;
        }
      }
      if (!found) {
        break;
      }
      matched.push_back(*found);
    }
    if (matched.size() > best.size()) {
      best = std::move(matched);
      best_index = x;
    }
  }

  // An equality on a sorted column finds the fewest rows for the least work,
  // then an index's equalities; a range of a sorted column is searched only
  // when there is neither.
  if (sorted_eq || (best.empty() && sorted_range)) {
    out.kind = Searches::Kind::kSorted;
    for (uint32_t p = 0; p < predicates.size(); ++p) {
      if (out.strategies[p]) {
        out.searched.push_back(p);
      }
    }
  } else if (!best.empty()) {
    out.kind = Searches::Kind::kIndex;
    out.searched = std::move(best);
    out.index = best_index;
  }
  return out;
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
    ASSIGN_OR_RETURN(analysis::RelationLineage lineage,
                     Scan::AnalyzeSource(c, source));
    std::vector<DataframeColumn> columns;
    if (const dataframe::Dataframe* renamed =
            Scan::FindRenamedDataframe(c, lineage, &columns)) {
      scan = Scan::BuildDataframeScan(
          c, *renamed, std::string(*lineage.row_origin()), columns);
    } else {
      scan = Scan::BuildSqlScan(c, source, lineage);
    }
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
    Scan::AddScanColumn(
        c, &scan, column.name,
        HoldsNoValues(dataframe.column(column.index), dataframe.row_count())
            ? PlanType::Null()
            : PlanType(dataframe.column_type(column.index)));
    source.column_names.push_back(dataframe.column_names()[column.index]);
    source.columns.push_back(dataframe.shared_column(column.index));
  }
  source.indexes = IndexesOf(dataframe);
  scan.source_ = std::move(source);
  return scan;
}

base::StatusOr<analysis::RelationLineage> Scan::AnalyzeSource(Compiler* c,
                                                              uint32_t from) {
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
  return lineage;
}

const dataframe::Dataframe* Scan::FindRenamedDataframe(
    Compiler* c,
    const analysis::RelationLineage& lineage,
    std::vector<DataframeColumn>* columns) {
  std::optional<std::string_view> origin = lineage.row_origin();
  if (!origin) {
    return nullptr;
  }
  const dataframe::Dataframe* dataframe = c->catalog().FindDataframe(*origin);
  // As for a table read directly, the dataframe must have stopped changing.
  if (!dataframe || !dataframe->finalized()) {
    return nullptr;
  }
  for (const analysis::ColumnLineage& column : lineage.columns()) {
    if (column.origins.size() != 1) {
      return nullptr;
    }
    std::optional<uint32_t> i =
        FindScanColumn(*dataframe, column.origins[0].column_name);
    if (!i) {
      return nullptr;
    }
    columns->push_back({std::string(column.output_name), *i});
  }
  return dataframe;
}

Scan Scan::BuildSqlScan(Compiler* c,
                        uint32_t from,
                        const analysis::RelationLineage& lineage) {
  Scan scan;
  // The relation as written, which the SQL building its dataframe reads.
  scan.source_ = c->SourceForNode(from);
  for (const analysis::ColumnLineage& column : lineage.columns()) {
    PlanType type = PlanType::Unknown();
    if (std::optional<analysis::ColumnType> traced = column.type()) {
      StorageType storage = sql_schema::ToStorageType(*traced);
      // An Id only means something in the table it indexes: read through
      // SQL, it is a plain number.
      if (storage.Is<core::Id>()) {
        storage = core::Uint32{};
      }
      type = storage;
    }
    Scan::AddScanColumn(c, &scan, std::string(column.output_name), type);
  }
  return scan;
}

void Scan::AddScanColumn(Compiler* c,
                         Scan* scan,
                         std::string name,
                         PlanType type) {
  ColumnId id = c->AddColumn(name, type);
  scan->columns_.push_back({std::move(name), id});
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

core::RowEstimate Scan::Estimate(PlanEstimator* e, const PlanNode&) const {
  return EstimateFiltered(e, {});
}

core::RowEstimate Scan::EstimateFiltered(
    PlanEstimator* e,
    const std::vector<WherePredicate>& predicates) const {
  const auto* data = std::get_if<Scan::Dataframe>(&source_);
  if (!data) {
    // SQL, read as a dataframe built when the query runs.
    e->AddCost(PlanEstimator::kReadCostPerRow * PlanEstimator::kAssumedRows);
    return e->ApplyBlind(
        {PlanEstimator::kAssumedRows, PlanEstimator::kAssumedRows}, predicates);
  }
  // As LowerSearching runs them: searches of sorted columns or an index,
  // then tests of the rows those find.
  Searches searches = PlanSearches(*data, columns_, predicates);
  core::RowModel model(data->row_count);
  auto apply = [&](uint32_t p) {
    const WherePredicate& predicate = predicates[p];
    if (!searches.positions[p]) {
      model.ApplyFilter(predicate.op, core::HasDuplicates{}, 0,
                        /*non_null=*/false);
      return;
    }
    const dataframe::Column& column = *data->columns[*searches.positions[p]];
    model.ApplyFilter(predicate.op, column.duplicate_state,
                      column.estimated_distinct,
                      column.null_storage.nullability().Is<core::NonNull>());
  };
  std::vector<bool> searched(predicates.size());
  for (uint32_t p : searches.searched) {
    e->AddSearchCost(searches.kind == Searches::Kind::kIndex
                         ? data->row_count
                         : model.rows().max);
    apply(p);
    searched[p] = true;
  }
  e->AddCost(PlanEstimator::kReadCostPerRow * model.rows().estimated);
  for (uint32_t p = 0; p < predicates.size(); ++p) {
    if (!searched[p]) {
      e->AddCost(PlanEstimator::kFilterCostPerRow * model.rows().estimated);
      apply(p);
    }
  }
  return model.rows();
}

std::optional<std::vector<bool>> Scan::LowerSearching(
    Lowering* c,
    const std::vector<WherePredicate>& predicates) const {
  const auto* data = std::get_if<Scan::Dataframe>(&source_);
  if (!data) {
    return std::nullopt;
  }
  Searches searches = PlanSearches(*data, columns_, predicates);
  std::vector<bool> searched(predicates.size());
  for (uint32_t p : searches.searched) {
    searched[p] = true;
  }
  // The values searched for are cast for the dataframe's columns, which is
  // how the scan holds them.
  auto value = [&](uint32_t p) {
    const dataframe::Column& column = *data->columns[*searches.positions[p]];
    return Where::LowerValue(c, predicates[p], column.storage.type());
  };

  PERFETTO_DCHECK(!c->has_source());
  for (const NamedColumn& column : columns_) {
    c->DeclareColumn(column.id);
  }
  switch (searches.kind) {
    case Searches::Kind::kNone:
      c->SetSource(MakeSource());
      break;
    case Searches::Kind::kIndex: {
      std::vector<ex::IndexScan::Equality> equalities;
      for (uint32_t p : searches.searched) {
        equalities.push_back({data->columns[*searches.positions[p]], value(p)});
      }
      const ScanIndex& index = data->indexes[searches.index];
      c->SetSource(std::make_unique<ex::IndexScan>(
          data->columns,
          dataframe::Index(std::vector<uint32_t>(index.column_names.size()),
                           index.permutation),
          std::move(equalities), c->context().pool));
      // The rows come in the index's order, in runs of rows which ascend.
      c->DeclareSmallBatches();
      return searched;
    }
    case Searches::Kind::kSorted: {
      uint32_t p = searches.searched[0];
      const auto& strategy = *searches.strategies[p];
      const auto& column = data->columns[*searches.positions[p]];
      bool one_eq =
          searches.searched.size() == 1 && predicates[p].op.Is<core::Eq>();
      if (one_eq && column->storage.type().Is<core::Id>()) {
        c->SetSource(std::make_unique<ex::IdEqScan>(data->columns,
                                                    data->row_count, value(p)));
      } else if (one_eq &&
                 std::holds_alternative<dataframe::logical::SetIdSortedSearch>(
                     strategy)) {
        c->SetSource(std::make_unique<ex::SetIdEqScan>(
            data->columns, data->row_count, column, value(p)));
      } else if (one_eq &&
                 std::holds_alternative<dataframe::logical::SmallValueLookup>(
                     strategy)) {
        c->SetSource(std::make_unique<ex::SmallValueEqScan>(
            data->columns, data->row_count, column, value(p)));
      } else {
        std::vector<ex::SortedScan::Search> sorted;
        for (uint32_t q : searches.searched) {
          ex::SortedScan::Search search;
          search.column = data->columns[*searches.positions[q]];
          const auto& s = *searches.strategies[q];
          search.kind =
              std::holds_alternative<dataframe::logical::SetIdSortedSearch>(s)
                  ? ex::SortedScan::Search::Kind::kSetIdEq
              : std::holds_alternative<dataframe::logical::SmallValueLookup>(s)
                  ? ex::SortedScan::Search::Kind::kSmallValueEq
                  : ex::SortedScan::Search::Kind::kBinarySearch;
          search.op = predicates[q].op;
          search.value = value(q);
          sorted.push_back(std::move(search));
        }
        c->SetSource(std::make_unique<ex::SortedScan>(
            data->columns, data->row_count, std::move(sorted),
            c->context().pool));
      }
      break;
    }
  }
  // A range of a dataframe is read in order, so a column it keeps sorted
  // ascends.
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    if (!data->columns[i]->sort_state.Is<core::Unsorted>()) {
      c->DeclareSorted({{columns_[i].id, false}});
    }
  }
  return searched;
}

void Scan::Lower(Lowering* c, const PlanNode&) const {
  const auto& scan = *this;

  PERFETTO_DCHECK(!c->has_source());
  for (const NamedColumn& column : scan.columns_) {
    c->DeclareColumn(column.id);
  }
  c->SetSource(scan.MakeSource());
  // A dataframe is read in order, so a column it keeps sorted ascends.
  const auto& dataframe = base::unchecked_get<Scan::Dataframe>(scan.source_);
  for (uint32_t i = 0; i < scan.columns_.size(); ++i) {
    if (!dataframe.columns[i]->sort_state.Is<core::Unsorted>()) {
      c->DeclareSorted({{scan.columns_[i].id, false}});
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
    const PlanType& type = plan.columns()[scan.columns_[j].id].type;
    bool same =
        i &&
        (type.is_null()
             ? HoldsNoValues(dataframe->column(*i), dataframe->row_count())
             : type.storage() && *type.storage() == dataframe->column_type(*i));
    if (!same) {
      return base::ErrStatus(
          "Pipeline: table '%s' has changed since the pipeline was written",
          data->name.c_str());
    }
    data->columns.push_back(dataframe->shared_column(*i));
  }
  data->row_count = dataframe->row_count();
  data->indexes = IndexesOf(*dataframe);

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
    plan.columns()[column.id].type =
        HoldsNoValues(dataframe->column(*i), dataframe->row_count())
            ? PlanType::Null()
            : PlanType(dataframe->column_type(*i));
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
