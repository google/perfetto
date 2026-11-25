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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/variant.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/row_estimate.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/empty_source.h"
#include "src/trace_processor/core/exec/index_scan.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/sorted_scan.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

namespace analysis = ::perfetto::perfetto_sql::analysis;
using core::StorageType;

namespace {

// The columns of `data` a scan lends sparse (see ColumnView::Kind::kSparse).
std::vector<uint32_t> SparseColumns(const Bindings::Dataframe& data) {
  std::vector<uint32_t> sparse;
  for (uint32_t i = 0; i < data.columns.size(); ++i) {
    const auto& n = data.columns[i]->null_storage.nullability();
    if (n.Is<core::SparseNull>() ||
        n.Is<core::SparseNullWithPopcountAlways>() ||
        n.Is<core::SparseNullWithPopcountUntilFinalization>()) {
      sparse.push_back(i);
    }
  }
  return sparse;
}

bool IsSubpipeline(Compiler* c, const SyntaqlitePerfettoPipeSource& n) {
  if (!syntaqlite_node_is_present(n.select)) {
    return false;
  }
  const auto* node = Node<SyntaqliteNode>(c->parser(), n.select);
  return node->tag == SYNTAQLITE_NODE_PERFETTO_PIPELINE;
}

bool HoldsNoValues(const dataframe::Column& column, uint32_t rows) {
  if (rows == 0) {
    return true;
  }
  const core::BitVector* present = column.null_storage.MaybeGetNullBitVector();
  return present && !present->AnyBitSet();
}

// The type a pipeline reads a dataframe column as.
// Id is a storage type only for searching: a column read is Uint32.
PlanType ColumnType(const dataframe::Dataframe& dataframe, uint32_t i) {
  if (HoldsNoValues(dataframe.column(i), dataframe.row_count())) {
    return PlanType::Null();
  }
  StorageType type = dataframe.column_type(i);
  return PlanType(type.Is<core::Id>() ? StorageType{core::Uint32{}} : type);
}

// The stamp of a table's name and of the index, name and type of each column
// a scan reads, by which a read plan tells whether the table has changed. In
// the host's byte order, as only the process which wrote it checks it.
class TableStamp {
 public:
  PERFETTO_NO_INLINE explicit TableStamp(std::string_view table) {
    AddString(table);
  }

  PERFETTO_NO_INLINE void AddColumn(uint32_t index,
                                    std::string_view name,
                                    const PlanType& type) {
    data_.append(reinterpret_cast<const char*>(&index), sizeof(index));
    AddString(name);
    PERFETTO_DCHECK(type.is_null() || type.storage());
    data_.push_back(
        type.is_null() ? '\0' : static_cast<char>(type.storage()->index() + 1));
  }

  uint64_t Stamp(const PlanKey& key) const {
    return key.Stamp(PlanKey::Use::kTable, data_);
  }

 private:
  void AddString(std::string_view text) {
    auto size = static_cast<uint32_t>(text.size());
    data_.append(reinterpret_cast<const char*>(&size), sizeof(size));
    data_.append(text);
  }

  std::string data_;
};

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

constexpr OperationRegistration Scan::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE, &BuildPlan,
    OperationRegistration::Encoding{0, &MakeOperation<Scan>}};

base::Status Scan::BuildPlan(Compiler* c, uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), from);
  ASSIGN_OR_RETURN(Builder::ScanResult relation, Scan::BuildRelation(c, from));
  c->SetRoot(relation.relation);
  for (NamedColumn& column : relation.columns) {
    c->Append(std::move(column), from);
  }
  if (std::optional<NameId> qualifier = Scan::SourceQualifier(c, *n)) {
    c->AddAlias(*qualifier, 0);
  }
  return base::OkStatus();
}

base::StatusOr<Builder::ScanResult> Scan::BuildRelation(Compiler* c,
                                                        uint32_t source) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), source);
  if (IsSubpipeline(c, *n)) {
    ASSIGN_OR_RETURN(Builder::ScanResult sub, c->CompileSubpipeline(n->select));
    RETURN_IF_ERROR(Scan::CheckSourceNames(c, sub.columns, source));
    return sub;
  }
  Builder::ScanResult out;
  if (const dataframe::Dataframe* dataframe =
          Scan::FindDirectDataframe(c, *n)) {
    // Hidden from SELECT * in SQLite, so hidden here too.
    std::vector<Builder::TableColumn> columns;
    const std::vector<std::string>& names = dataframe->column_names();
    for (uint32_t i = 0; i < names.size(); ++i) {
      if (!dataframe::IsHiddenColumn(names[i])) {
        columns.push_back({c->Name(names[i]), i});
      }
    }
    out = c->builder().ScanTable(*dataframe,
                                 SpanText(c->parser(), n->table_name), columns);
  } else {
    ASSIGN_OR_RETURN(analysis::RelationLineage lineage,
                     Scan::AnalyzeSource(c, source));
    std::vector<Builder::TableColumn> columns;
    if (const dataframe::Dataframe* renamed =
            Scan::FindRenamedDataframe(c, lineage, &columns)) {
      out = c->builder().ScanTable(*renamed, *lineage.row_origin(), columns);
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
        sql_cols.push_back({c->Name(col.output_name), type});
      }
      out = c->builder().ScanSql(c->SourceForNode(source).sql(), sql_cols);
    }
  }
  RETURN_IF_ERROR(Scan::CheckSourceNames(c, out.columns, source));
  return out;
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

std::optional<NameId> Scan::SourceQualifier(
    Compiler* c,
    const SyntaqlitePerfettoPipeSource& n) {
  // As in SQLite, an alias replaces the table name.
  if (syntaqlite_node_is_present(n.alias)) {
    const auto* alias = Node<SyntaqliteName>(c->parser(), n.alias);
    return c->Name(alias->ident_name.source);
  }
  if (!syntaqlite_node_is_present(n.select)) {
    return c->Name(n.table_name);
  }
  return std::nullopt;
}

base::Status Scan::CheckSourceNames(Compiler* c,
                                    base::Span<const NamedColumn> columns,
                                    uint32_t at) {
  for (size_t i = 0; i < columns.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (c->Same(columns[j].name, columns[i].name)) {
        return c->Expected(at,
                           "distinct column names, but there are two named '" +
                               c->Text(columns[j].name) + "'");
      }
    }
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string& name = c->Text(columns[i].name);
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

Bindings::Dataframe Scan::BindingFor(const dataframe::Dataframe& dataframe,
                                     const ScanDataframe& data) {
  Bindings::Dataframe bound;
  PERFETTO_NO_VECTORIZE_LOOP
  for (const ScanDataframeColumn& column : data.read) {
    bound.columns.push_back(dataframe.shared_column(column.index));
    bound.column_indices.push_back(column.index);
  }
  bound.row_count = dataframe.row_count();
  for (const dataframe::Index& index : dataframe.indexes()) {
    bound.indexes.push_back(index.Copy());
  }
  return bound;
}

PlanType Scan::ColumnTypeFor(const dataframe::Dataframe& dataframe,
                             uint32_t index) {
  return ColumnType(dataframe, index);
}

PlanNodeId Scan::AddToPlan(LogicalPlan* plan,
                           const dataframe::Dataframe& dataframe,
                           std::string_view name,
                           const std::vector<uint32_t>& columns) {
  core::Arena& arena = plan->arena();
  auto count = static_cast<uint32_t>(columns.size());
  Scan scan;
  scan.columns_ = arena.NewSpan<NamedColumn>(count);
  Scan::Dataframe source;
  source.name = arena.CopyString(name);
  source.read = arena.NewSpan<ScanDataframeColumn>(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t index = columns[i];
    NameId column_name = plan->names().Intern(dataframe.column_names()[index]);
    scan.columns_[i] = {
        column_name,
        plan->AddColumn(column_name, ColumnType(dataframe, index))};
    source.read[i] = {index, column_name};
  }
  scan.source_ = source;
  return plan->AddNode(scan);
}

void Scan::Bind(BoundPlan* bound,
                PlanNodeId node,
                const dataframe::Dataframe& dataframe) {
  const auto& scan = bound->plan.nodes()[node].Cast<Scan>();
  bound->bindings.dataframes[node] =
      BindingFor(dataframe, std::get<Scan::Dataframe>(scan.source_));
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
    std::vector<Builder::TableColumn>* columns) {
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
    columns->push_back({c->Name(column.output_name), *i});
  }
  return dataframe;
}

std::optional<uint32_t> Scan::Prune(std::vector<bool>* needed) {
  auto* dataframe = std::get_if<Scan::Dataframe>(&source_);
  auto* arg = std::get_if<Scan::DataframeArg>(&source_);
  bool has_arg_read = arg && !arg->read.empty();
  uint32_t kept = 0;
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    if ((*needed)[columns_[i].id]) {
      columns_[kept] = columns_[i];
      if (dataframe) {
        dataframe->read[kept] = dataframe->read[i];
      } else if (has_arg_read) {
        arg->read[kept] = arg->read[i];
      }
      ++kept;
    }
  }
  // Always keep at least one column: a batch with no columns has no rows.
  if (kept == 0 && !columns_.empty()) {
    kept = 1;
  }
  columns_.Shrink(kept);
  if (dataframe) {
    dataframe->read.Shrink(kept);
  } else if (has_arg_read) {
    arg->read.Shrink(kept);
  }
  return std::nullopt;
}

OutputProperties Scan::Derive(DeriveContext& c) const {
  OutputProperties out;
  for (const NamedColumn& column : columns_) {
    out.columns.push_back(c.ColumnOf(column.id));
  }
  const std::optional<Bindings::Dataframe>& data =
      c.bound().bindings.dataframes[c.self()];
  // An index is read in its own order, not the table's.
  if (search_ && search_->kind == ScanSearch::Kind::kIndex) {
    return out;
  }
  // A dataframe is read in order, so a column it keeps sorted ascends.
  for (uint32_t i = 0; data && i < columns_.size(); ++i) {
    if (!data->columns[i]->sort_state.Is<core::Unsorted>()) {
      out.arrangements.push_back({{}, {{columns_[i].id, false}}});
    }
  }
  if (data) {
    out.reversible_scan = c.self();
  }
  return out;
}

void Scan::Lower(Lowering* c, const PlanNode& node) const {
  PERFETTO_DCHECK(!c->has_source());
  // SQL is moved out into dataframe arguments, and every scan is bound to a
  // dataframe, before a plan is run.
  const Bindings::Dataframe& data = c->QueryDataframe(node);
  if (search_) {
    LowerSearch(c, node, data);
    return;
  }
  c->SetSource(c->arena().New<ex::DataframeScan>(c->KeepColumns(data.columns),
                                                 data.row_count,
                                                 c->QueryScanDirection(node)));
  c->SetSparseColumns(SparseColumns(data));
}

void Scan::LowerSearch(Lowering* c,
                       const PlanNode& node,
                       const Bindings::Dataframe& data) const {
  if (search_->kind == ScanSearch::Kind::kEmpty) {
    c->SetSource(c->arena().New<ex::EmptySource>());
    return;
  }
  const StringPool* pool = c->context().pool;
  // The column each searched condition is on, and its value for this run.
  std::vector<const dataframe::Column*> columns;
  std::vector<uint32_t> values;
  for (uint32_t i = 0; i < search_->predicates.size(); ++i) {
    columns.push_back(
        data.columns[Position(search_->predicates[i].column)].get());
    values.push_back(*c->TakeParam(node, i));
  }
  // The searches of sorted columns come first: all of them for kSorted.
  uint32_t sorted = search_->kind == ScanSearch::Kind::kIndex
                        ? search_->sorted
                        : static_cast<uint32_t>(columns.size());
  core::Arena& arena = c->arena();
  auto searches = arena.NewSpan<ex::SortedScan::Search>(sorted);
  for (uint32_t i = 0; i < sorted; ++i) {
    ex::SortedScan::Search search;
    search.column = columns[i];
    search.op = search_->predicates[i].op;
    std::optional<dataframe::logical::FilterStrategy> strategy =
        dataframe::SortedFilterStrategy(*columns[i], search.op);
    PERFETTO_CHECK(strategy);
    if (std::holds_alternative<dataframe::logical::SetIdSortedSearch>(
            *strategy)) {
      search.kind = ex::SortedScan::Search::Kind::kSetIdEq;
    } else if (std::holds_alternative<dataframe::logical::SmallValueLookup>(
                   *strategy)) {
      search.kind = ex::SortedScan::Search::Kind::kSmallValueEq;
    } else {
      PERFETTO_DCHECK(
          std::holds_alternative<dataframe::logical::BinarySearch>(*strategy));
      search.kind = ex::SortedScan::Search::Kind::kBinarySearch;
    }
    search.param = values[i];
    searches[i] = search;
  }
  if (search_->kind == ScanSearch::Kind::kIndex) {
    // Rows are read in index order, so the table's order is lost.
    PERFETTO_DCHECK(c->QueryScanDirection(node) ==
                    ex::ReadDirection::kForwards);
    auto equalities = arena.NewSpan<ex::IndexScan::Equality>(
        static_cast<uint32_t>(columns.size()) - sorted);
    for (uint32_t i = sorted; i < columns.size(); ++i) {
      equalities[i - sorted] = {columns[i], values[i]};
    }
    const core::dataframe::Index& index = data.indexes[search_->index];
    c->KeepAlive(index.permutation_vector());
    c->SetSource(arena.New<ex::IndexScan>(arena, c->KeepColumns(data.columns),
                                          index, equalities, searches, pool));
    // It copies the rows it reads, so lends no sparse column.
    return;
  }
  c->SetSource(arena.New<ex::SortedScan>(arena, c->KeepColumns(data.columns),
                                         data.row_count, searches, pool,
                                         c->QueryScanDirection(node)));
  c->SetSparseColumns(SparseColumns(data));
}

uint32_t Scan::Position(ColumnId column) const {
  auto it = std::find_if(
      columns_.begin(), columns_.end(),
      [&](const NamedColumn& named) { return named.id == column; });
  PERFETTO_CHECK(it != columns_.end());
  return static_cast<uint32_t>(it - columns_.begin());
}

std::optional<Scan::SearchPlan> Scan::PlanSearch(
    const std::vector<Condition>& conditions,
    const Bindings::Dataframe& data) const {
  if (conditions.empty()) {
    return std::nullopt;
  }
  SearchPlan plan;
  // A condition no row can match leaves nothing to read.
  for (const Condition& condition : conditions) {
    if (condition.value &&
        condition.value->validity ==
            core::filter::CastFilterValueResult::kNoneMatch) {
      plan.kind = ScanSearch::Kind::kEmpty;
      for (uint32_t i = 0; i < conditions.size(); ++i) {
        plan.answered.push_back(i);
      }
      return plan;
    }
  }

  // Each sorted column narrows the rows to a range, which stays in order. As
  // in the dataframe's planner, an equality on a column with no duplicates
  // leaves at most one row, which no index is worth searching for.
  std::vector<uint8_t> unsearchable(conditions.size(), false);
  core::RowModel rows(data.row_count);
  for (uint32_t i = 0; i < conditions.size(); ++i) {
    const dataframe::Column& column =
        *data.columns[Position(conditions[i].column)];
    if (!dataframe::SortedFilterStrategy(column, conditions[i].op)) {
      continue;
    }
    plan.answered.push_back(i);
    unsearchable[i] = true;
    if (conditions[i].op.Is<core::Eq>()) {
      rows.ApplyEqualityFilter(column.duplicate_state,
                               column.estimated_distinct);
    }
  }
  auto sorted = static_cast<uint32_t>(plan.answered.size());

  // An index then finds equalities on its leading columns, in index order
  // rather than the dataframe's.
  if (!data.indexes.empty()) {
    std::vector<dataframe::FilterSpec> specs;
    for (uint32_t i = 0; i < conditions.size(); ++i) {
      uint32_t position = Position(conditions[i].column);
      specs.push_back({data.column_indices[position], i, conditions[i].op, {}});
      bool id = data.columns[position]->storage.type().Is<core::Id>();
      unsearchable[i] |= !conditions[i].op.Is<core::Eq>() || id;
    }
    if (std::optional<dataframe::BestIndex> best =
            dataframe::GetBestIndexForFilterSpecs(rows.rows().max, specs,
                                                  unsearchable, data.indexes)) {
      plan.kind = ScanSearch::Kind::kIndex;
      plan.index = best->best_index_idx;
      plan.sorted = sorted;
      plan.answered.insert(plan.answered.end(), best->best_index_specs.begin(),
                           best->best_index_specs.end());
      return plan;
    }
  }
  if (sorted == 0) {
    return std::nullopt;
  }
  plan.kind = ScanSearch::Kind::kSorted;
  return plan;
}

const OperationRegistration& Scan::registration() const {
  return kRegistration;
}

base::Status Scan::ResolveDataframes(const LogicalPlan&,
                                     PlanNodeId self,
                                     const Catalog* catalog,
                                     const PlanKey* key,
                                     Bindings* bindings) const {
  auto* data = std::get_if<Scan::Dataframe>(&source_);
  if (!data) {
    return base::OkStatus();
  }
  if (!catalog) {
    return base::ErrStatus(
        "Pipeline: cannot resolve table '%.*s' without a catalog",
        static_cast<int>(data->name.size()), data->name.data());
  }
  const dataframe::Dataframe* dataframe = catalog->FindDataframe(data->name);
  if (!dataframe) {
    return base::ErrStatus("Pipeline: table '%.*s' no longer exists",
                           static_cast<int>(data->name.size()),
                           data->name.data());
  }
  // A plan written into SQL may be read after the table changed.
  if (key) {
    const std::vector<std::string>& names = dataframe->column_names();
    TableStamp stamp(data->name);
    bool in_range = true;
    PERFETTO_NO_VECTORIZE_LOOP
    for (const ScanDataframeColumn& column : data->read) {
      uint32_t i = column.index;
      in_range = in_range && i < names.size();
      if (in_range) {
        stamp.AddColumn(i, names[i], ColumnType(*dataframe, i));
      }
    }
    if (!in_range || stamp.Stamp(*key) != data->stamp) {
      return base::ErrStatus(
          "Pipeline: table '%.*s' has changed since the pipeline was written",
          static_cast<int>(data->name.size()), data->name.data());
    }
  }
  bindings->dataframes[self] = BindingFor(*dataframe, *data);
  return base::OkStatus();
}

base::Status Scan::BindDataframeArgs(
    const LogicalPlan& plan,
    PlanNodeId self,
    base::Span<const dataframe::Dataframe* const> args,
    StringPool* pool,
    Bindings* bindings) const {
  const auto& scan = *this;
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
  bool synthesized_empty = dataframe == nullptr;
  bool by_index = !synthesized_empty && !arg->read.empty();
  if (synthesized_empty) {
    std::vector<std::string> names;
    for (const NamedColumn& column : scan.columns_) {
      names.push_back(plan.names().text(column.name));
    }
    dataframe::AdhocDataframeBuilder::Options options;
    options.emit_auto_id = false;
    ASSIGN_OR_RETURN(
        empty, dataframe::AdhocDataframeBuilder(std::move(names), pool, options)
                   .Build());
    dataframe = &*empty;
  }
  if (by_index && arg->read.size() != scan.columns_.size()) {
    return base::ErrStatus(
        "Pipeline: invalid column mapping for dataframe argument %u",
        arg->index);
  }
  Bindings::Dataframe data;
  data.columns.reserve(scan.columns_.size());
  data.column_indices.reserve(scan.columns_.size());
  for (uint32_t pos = 0; pos < scan.columns_.size(); ++pos) {
    const NamedColumn& column = scan.columns_[pos];
    uint32_t df_col = 0;
    if (synthesized_empty) {
      df_col = pos;
    } else if (by_index) {
      df_col = arg->read[pos];
      if (df_col >= dataframe->column_count()) {
        return base::ErrStatus(
            "Pipeline: dataframe argument %u column index %u out of range",
            arg->index, df_col);
      }
    } else {
      const std::string& name = plan.names().text(column.name);
      std::optional<uint32_t> i = FindScanColumn(*dataframe, name);
      if (!i) {
        return base::ErrStatus(
            "Pipeline: dataframe argument %u has no column '%s'", arg->index,
            name.c_str());
      }
      df_col = *i;
    }
    // The dataframe may have been built or mutated after the plan was written,
    // so it decides what each column holds.
    bindings->types[column.id] = ColumnType(*dataframe, df_col);
    data.columns.push_back(dataframe->shared_column(df_col));
    data.column_indices.push_back(df_col);
  }
  data.row_count = dataframe->row_count();
  for (const dataframe::Index& index : dataframe->indexes()) {
    data.indexes.push_back(index.Copy());
  }
  bindings->dataframes[self] = std::move(data);

  return base::OkStatus();
}

void Scan::MoveSqlSourcesToDataframeArgs(const NameTable& table,
                                         std::vector<std::string>* sql_args) {
  auto& args = *sql_args;
  auto& scan = *this;
  if (!std::holds_alternative<std::string_view>(scan.source_)) {
    return;
  }
  std::vector<std::string> names;
  std::vector<std::string> references;
  for (const NamedColumn& column : scan.columns_) {
    names.push_back(table.text(column.name));
    references.push_back(QuoteIdentifier(table.text(column.name)));
  }
  std::string_view from = base::unchecked_get<std::string_view>(scan.source_);
  args.push_back("SELECT " + std::string(kDataframeAggFunction) + "(" +
                 QuoteString(base::Join(names, ",")) + ", " +
                 base::Join(references, ", ") + ") FROM " + std::string(from));
  scan.source_ = Scan::DataframeArg{static_cast<uint32_t>(args.size() - 1), {}};
}

void Scan::Serialize(PlanCodec& c) {
  // Only a bound plan, which is never written, has a search.
  PERFETTO_DCHECK(!search_);
  auto kind = static_cast<uint32_t>(source_.index());
  c.Pod(&kind);
  if (c.reading()) {
    if (kind == base::variant_index<Source, Dataframe>()) {
      source_ = Dataframe();
    } else if (kind == base::variant_index<Source, std::string_view>()) {
      source_ = std::string_view();
    } else {
      source_ = DataframeArg();
    }
  }
  c.Vec(&columns_);
  if (auto* sql = std::get_if<std::string_view>(&source_)) {
    c.Str(sql);
    return;
  }
  if (auto* arg = std::get_if<DataframeArg>(&source_)) {
    c.Pod(&arg->index);
    c.Vec(&arg->read);
    return;
  }
  auto& data = std::get<Dataframe>(source_);
  c.Str(&data.name);
  c.Vec(&data.read);
  uint64_t stamp = data.stamp;
  if (!c.reading()) {
    if (c.has_key()) {
      TableStamp table_stamp(data.name);
      PERFETTO_NO_VECTORIZE_LOOP
      for (uint32_t i = 0; i < columns_.size(); ++i) {
        table_stamp.AddColumn(data.read[i].index,
                              c.plan().names().text(data.read[i].name),
                              c.plan().columns()[columns_[i].id].type);
      }
      stamp = table_stamp.Stamp(c.key());
    } else {
      stamp = 0;
    }
  }
  c.Pod(&stamp);
  if (c.reading()) {
    data.stamp = stamp;
  }
}

}  // namespace perfetto::trace_processor::pipeline
