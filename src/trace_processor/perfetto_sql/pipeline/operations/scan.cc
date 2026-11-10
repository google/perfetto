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

const OperationRegistration Scan::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE, &BuildPlan,
    OperationRegistration::Encoding{0, &MakeOperation<Scan>}};

base::Status Scan::BuildPlan(Compiler* c, uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), from);
  ASSIGN_OR_RETURN(SourceRelation relation, Scan::BuildRelation(c, from));
  for (NamedColumn& column : relation.columns) {
    c->Append(std::move(column), from);
  }
  if (std::optional<NameId> qualifier = Scan::SourceQualifier(c, *n)) {
    c->AddAlias({*qualifier, c->row()});
  }
  return base::OkStatus();
}

base::StatusOr<SourceRelation> Scan::BuildRelation(Compiler* c,
                                                   uint32_t source) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(c->parser(), source);
  if (IsSubpipeline(c, *n)) {
    SourceRelation relation;
    ASSIGN_OR_RETURN(relation.columns, c->CompileSubpipeline(n->select));
    RETURN_IF_ERROR(Scan::CheckSourceNames(c, relation.columns, source));
    relation.node = c->plan().root();
    return relation;
  }
  Scan scan;
  if (const dataframe::Dataframe* dataframe =
          Scan::FindDirectDataframe(c, *n)) {
    // Hidden from SELECT * in SQLite, so hidden here too.
    std::vector<DataframeColumn> columns;
    const std::vector<std::string>& names = dataframe->column_names();
    for (uint32_t i = 0; i < names.size(); ++i) {
      if (!dataframe::IsHiddenColumn(names[i])) {
        columns.push_back({c->Name(names[i]), i});
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
                                    const std::vector<NamedColumn>& columns,
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
  }
  bound.row_count = dataframe.row_count();
  for (const dataframe::Index& index : dataframe.indexes()) {
    bound.indexes.push_back(index.Copy());
  }
  return bound;
}

PlanNodeId Scan::AddToPlan(LogicalPlan* plan,
                           const dataframe::Dataframe& dataframe,
                           std::string name,
                           const std::vector<uint32_t>& columns) {
  Scan scan;
  Scan::Dataframe source;
  source.name = std::move(name);
  for (uint32_t i : columns) {
    NameId column_name = plan->names().Intern(dataframe.column_names()[i]);
    scan.columns_.push_back(
        {column_name, plan->AddColumn(column_name, ColumnType(dataframe, i))});
    source.read.push_back({i, column_name});
  }
  scan.source_ = std::move(source);
  return plan->AddNode(std::move(scan));
}

void Scan::Bind(BoundPlan* bound,
                PlanNodeId node,
                const dataframe::Dataframe& dataframe) {
  const auto& scan = bound->plan.nodes()[node].Cast<Scan>();
  bound->bindings.dataframes[node] =
      BindingFor(dataframe, std::get<Scan::Dataframe>(scan.source_));
}

Scan Scan::BuildDataframeScan(Compiler* c,
                              const dataframe::Dataframe& dataframe,
                              std::string name,
                              const std::vector<DataframeColumn>& columns) {
  Scan scan;
  Scan::Dataframe source;
  source.name = std::move(name);
  for (const DataframeColumn& column : columns) {
    Scan::AddScanColumn(c, &scan, column.name,
                        ColumnType(dataframe, column.index));
    source.read.push_back(
        {column.index, c->Name(dataframe.column_names()[column.index])});
  }
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
    columns->push_back({c->Name(column.output_name), *i});
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
    Scan::AddScanColumn(c, &scan, c->Name(column.output_name), type);
  }
  return scan;
}

void Scan::AddScanColumn(Compiler* c, Scan* scan, NameId name, PlanType type) {
  scan->columns_.push_back({name, c->AddColumn(name, type)});
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
      std::vector<ScanDataframeColumn> kept_read;
      for (uint32_t i : keep) {
        kept_read.push_back(dataframe.read[i]);
      }
      dataframe.read = std::move(kept_read);
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
  c->SetSource(std::make_unique<ex::DataframeScan>(
      data.columns, data.row_count, c->QueryScanDirection(node)));
  c->SetSparseColumns(SparseColumns(data));
}

void Scan::LowerSearch(Lowering* c,
                       const PlanNode& node,
                       const Bindings::Dataframe& data) const {
  if (search_->kind == ScanSearch::Kind::kEmpty) {
    c->SetSource(std::make_unique<ex::EmptySource>());
    return;
  }
  const StringPool* pool = c->context().pool;
  // The column each searched condition is on, and its value for this run.
  std::vector<std::shared_ptr<const dataframe::Column>> columns;
  std::vector<uint32_t> values;
  for (uint32_t i = 0; i < search_->predicates.size(); ++i) {
    columns.push_back(data.columns[Position(search_->predicates[i].column)]);
    values.push_back(*c->TakeParam(node, i));
  }
  // The searches of sorted columns come first: all of them for kSorted.
  uint32_t sorted = search_->kind == ScanSearch::Kind::kIndex
                        ? search_->sorted
                        : static_cast<uint32_t>(columns.size());
  std::vector<ex::SortedScan::Search> searches;
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
    searches.push_back(std::move(search));
  }
  if (search_->kind == ScanSearch::Kind::kIndex) {
    // Rows are read in index order, so the table's order is lost.
    PERFETTO_DCHECK(c->QueryScanDirection(node) ==
                    ex::ReadDirection::kForwards);
    std::vector<ex::IndexScan::Equality> equalities;
    for (uint32_t i = sorted; i < columns.size(); ++i) {
      equalities.push_back({columns[i], values[i]});
    }
    c->SetSource(std::make_unique<ex::IndexScan>(
        data.columns, data.indexes[search_->index], std::move(equalities),
        std::move(searches), pool));
    // It copies the rows it reads, so lends no sparse column.
    return;
  }
  c->SetSource(std::make_unique<ex::SortedScan>(data.columns, data.row_count,
                                                std::move(searches), pool,
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
        condition.value->value.validity ==
            core::filter::CastFilterValueResult::kNoneMatch) {
      plan.kind = ScanSearch::Kind::kEmpty;
      for (uint32_t i = 0; i < conditions.size(); ++i) {
        plan.answered.push_back(i);
      }
      return plan;
    }
  }

  // Each sorted column narrows the rows to a range, which stays in order.
  std::vector<uint8_t> unsearchable(conditions.size(), false);
  for (uint32_t i = 0; i < conditions.size(); ++i) {
    if (dataframe::SortedFilterStrategy(
            *data.columns[Position(conditions[i].column)], conditions[i].op)) {
      plan.answered.push_back(i);
      unsearchable[i] = true;
    }
  }
  auto sorted = static_cast<uint32_t>(plan.answered.size());

  // An index then finds equalities on its leading columns, in index order
  // rather than the dataframe's. Only a scan of a table has indexes.
  if (const auto* table = std::get_if<Scan::Dataframe>(&source_)) {
    std::vector<dataframe::FilterSpec> specs;
    for (uint32_t i = 0; i < conditions.size(); ++i) {
      uint32_t position = Position(conditions[i].column);
      specs.push_back({table->read[position].index, i, conditions[i].op, {}});
      bool id = data.columns[position]->storage.type().Is<core::Id>();
      unsearchable[i] |= !conditions[i].op.Is<core::Eq>() || id;
    }
    if (std::optional<dataframe::BestIndex> best =
            dataframe::GetBestIndexForFilterSpecs(data.row_count, specs,
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

std::unique_ptr<PlanOperation> Scan::Clone() const {
  return std::make_unique<Scan>(*this);
}

base::Status Scan::ResolveDataframes(const LogicalPlan&,
                                     PlanNodeId self,
                                     const Catalog& catalog,
                                     const PlanKey* key,
                                     Bindings* bindings) const {
  auto* data = std::get_if<Scan::Dataframe>(&source_);
  if (!data) {
    return base::OkStatus();
  }
  const dataframe::Dataframe* dataframe = catalog.FindDataframe(data->name);
  if (!dataframe) {
    return base::ErrStatus("Pipeline: table '%s' no longer exists",
                           data->name.c_str());
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
          "Pipeline: table '%s' has changed since the pipeline was written",
          data->name.c_str());
    }
  }
  bindings->dataframes[self] = BindingFor(*dataframe, *data);
  return base::OkStatus();
}

base::Status Scan::BindDataframeArgs(
    const LogicalPlan& plan,
    PlanNodeId self,
    const std::vector<const dataframe::Dataframe*>& args,
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
  if (!dataframe) {
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
  Bindings::Dataframe data;
  for (const NamedColumn& column : scan.columns_) {
    const std::string& name = plan.names().text(column.name);
    std::optional<uint32_t> i = FindScanColumn(*dataframe, name);
    if (!i) {
      return base::ErrStatus(
          "Pipeline: dataframe argument %u has no column '%s'", arg->index,
          name.c_str());
    }
    // The dataframe was built after the plan was written, so it decides
    // what each column holds.
    bindings->types[column.id] = ColumnType(*dataframe, *i);
    data.columns.push_back(dataframe->shared_column(*i));
  }
  data.row_count = dataframe->row_count();
  bindings->dataframes[self] = std::move(data);

  return base::OkStatus();
}

void Scan::MoveSqlSourcesToDataframeArgs(const NameTable& table,
                                         std::vector<std::string>* sql_args) {
  auto& args = *sql_args;
  auto& scan = *this;
  if (!std::holds_alternative<SqlSource>(scan.source_)) {
    return;
  }
  std::vector<std::string> names;
  std::vector<std::string> references;
  for (const NamedColumn& column : scan.columns_) {
    names.push_back(table.text(column.name));
    references.push_back(QuoteIdentifier(table.text(column.name)));
  }
  const std::string& from = base::unchecked_get<SqlSource>(scan.source_).sql();
  args.push_back("SELECT " + std::string(kDataframeAggFunction) + "(" +
                 QuoteString(base::Join(names, ",")) + ", " +
                 base::Join(references, ", ") + ") FROM " + from);
  scan.source_ = Scan::DataframeArg{static_cast<uint32_t>(args.size() - 1)};
}

void Scan::Serialize(PlanCodec& c) {
  // Only a bound plan, which is never written, has a search.
  PERFETTO_DCHECK(!search_);
  // SQL is moved out into dataframe arguments before a plan is written.
  auto kind = static_cast<uint32_t>(source_.index());
  c.Pod(&kind);
  if (c.reading()) {
    if (kind == base::variant_index<Source, Dataframe>()) {
      source_ = Dataframe();
    } else {
      source_ = DataframeArg();
    }
  }
  c.Vec(&columns_);
  auto* data = std::get_if<Dataframe>(&source_);
  if (!data) {
    c.Pod(&std::get<DataframeArg>(source_).index);
    return;
  }
  c.Str(&data->name);
  c.Vec(&data->read);
  if (!c.reading()) {
    TableStamp stamp(data->name);
    PERFETTO_NO_VECTORIZE_LOOP
    for (uint32_t i = 0; i < columns_.size(); ++i) {
      stamp.AddColumn(data->read[i].index,
                      c.plan().names().text(data->read[i].name),
                      c.plan().columns()[columns_[i].id].type);
    }
    data->stamp = stamp.Stamp(c.key());
  }
  c.Pod(&data->stamp);
}

}  // namespace perfetto::trace_processor::pipeline
