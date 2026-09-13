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

#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

std::string SpanText(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  uint32_t len;
  const char* text = syntaqlite_parser_span_expanded_text(p, &span, &len);
  PERFETTO_CHECK(text != nullptr);
  return {text, len};
}

template <typename T>
const T* Node(SyntaqliteParser* p, uint32_t id) {
  return static_cast<const T*>(syntaqlite_parser_node(p, id));
}

class Compiler {
 public:
  Compiler(SyntaqliteParser* p, const NodeSourceFn& source, const Catalog& c)
      : p_(p), source_(source), catalog_(c) {}

  base::Status CompileSource(uint32_t from);
  base::Status CompileStage(uint32_t stage);
  LogicalPlan Finish();

 private:
  op::Scan CompileDataframeSource(const dataframe::Dataframe&,
                                  std::string name);
  base::StatusOr<op::Scan> CompileSqlSource(uint32_t from);
  void AddScanColumn(op::Scan&, ColumnSchema);
  base::Status CompileTreeAccumulate(uint32_t stage);
  base::StatusOr<ColumnId> Resolve(const std::string& name,
                                   uint32_t at,
                                   const char* op) const;
  void Bind(NamedColumn column) {
    names_[base::ToLower(column.name)].push_back(column.id);
    plan_.output.push_back(std::move(column));
  }
  std::string Traceback(uint32_t node) const {
    return source_(node).AsTraceback(0);
  }

  SyntaqliteParser* p_;
  const NodeSourceFn& source_;
  const Catalog& catalog_;
  LogicalPlan plan_;
  base::FlatHashMap<std::string, std::vector<ColumnId>> names_;
};

base::Status Compiler::CompileSource(uint32_t from) {
  const auto* n = Node<SyntaqlitePerfettoPipeSource>(p_, from);
  op::Scan scan;

  bool bare_name = !syntaqlite_node_is_present(n->select) &&
                   n->schema.length == 0 &&
                   !syntaqlite_node_is_present(n->alias);
  std::string table = bare_name ? SpanText(p_, n->table_name) : "";
  const dataframe::Dataframe* dataframe =
      bare_name ? catalog_.FindDataframe(table) : nullptr;
  if (dataframe && dataframe->finalized()) {
    scan = CompileDataframeSource(*dataframe, std::move(table));
  } else {
    ASSIGN_OR_RETURN(scan, CompileSqlSource(from));
  }
  for (const NamedColumn& column : scan.columns) {
    Bind(column);
  }
  plan_.ops.emplace_back(std::move(scan));
  return base::OkStatus();
}

op::Scan Compiler::CompileDataframeSource(const dataframe::Dataframe& dataframe,
                                          std::string name) {
  op::Scan scan;
  op::Scan::Dataframe source;
  source.name = std::move(name);
  source.row_count = dataframe.row_count();
  const std::vector<std::string>& names = dataframe.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    // TODO(lalitm): Share SQL column visibility metadata with DataframeModule
    // instead of recognizing hidden columns by name here.
    // Hidden from SELECT * in SQLite, so hidden here too.
    if (names[i] == "_auto_id") {
      continue;
    }
    AddScanColumn(scan, {names[i], dataframe.column_type(i)});
    source.columns.push_back(dataframe.shared_column(i));
  }
  scan.source = std::move(source);
  return scan;
}

base::StatusOr<op::Scan> Compiler::CompileSqlSource(uint32_t from) {
  SqlSource sql = source_(from);
  sql =
      sql.RewriteAllIgnoreExisting(SqlSource::FromTraceProcessorImplementation(
          "SELECT * FROM " + sql.sql()));
  auto described = catalog_.DescribeQuery(sql);
  if (!described.ok()) {
    return base::ErrStatus("%s%s", Traceback(from).c_str(),
                           described.status().c_message());
  }
  op::Scan scan;
  scan.source = std::move(sql);
  for (ColumnSchema& column : *described) {
    AddScanColumn(scan, std::move(column));
  }
  return scan;
}

void Compiler::AddScanColumn(op::Scan& scan, ColumnSchema column) {
  ColumnId id = plan_.AddColumn(column.name, column.type);
  scan.columns.push_back({std::move(column.name), id});
}

base::Status Compiler::CompileStage(uint32_t stage) {
  const auto* node = Node<SyntaqliteNode>(p_, stage);
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE:
      return CompileTreeAccumulate(stage);
    default:
      PERFETTO_FATAL("Unknown pipeline stage");
  }
}

base::StatusOr<ColumnId> Compiler::Resolve(const std::string& name,
                                           uint32_t at,
                                           const char* op) const {
  const auto* matches = names_.Find(base::ToLower(name));
  if (!matches) {
    return base::ErrStatus("%s%s: no such column: '%s'", Traceback(at).c_str(),
                           op, name.c_str());
  }
  if (matches->size() != 1) {
    return base::ErrStatus("%s%s: column '%s' is ambiguous",
                           Traceback(at).c_str(), op, name.c_str());
  }
  return matches->front();
}

base::Status Compiler::CompileTreeAccumulate(uint32_t stage) {
  constexpr char kOp[] = "TREE ACCUMULATE";
  const auto* n = Node<SyntaqlitePerfettoTreeAccumulate>(p_, stage);

  op::TreeAccumulate acc;
  acc.direction = n->direction == SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN
                      ? op::TreeDirection::kDown
                      : op::TreeDirection::kUp;
  ASSIGN_OR_RETURN(acc.node_column, Resolve("id", stage, kOp));
  ASSIGN_OR_RETURN(acc.parent_column, Resolve("parent_id", stage, kOp));

  std::vector<NamedColumn> output;
  const auto* list =
      Node<SyntaqlitePerfettoTreeAggregateList>(p_, n->aggregates);
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoTreeAggregate>(p_, agg_id);
    std::string function = base::ToUpper(SpanText(p_, agg->function));
    if (function != "SUM" || agg->is_star == SYNTAQLITE_BOOL_TRUE) {
      return base::ErrStatus(
          "%s%s: only SUM(column) is supported so far, not %s",
          Traceback(agg_id).c_str(), kOp, function.c_str());
    }
    std::string column = SpanText(p_, agg->column);
    ASSIGN_OR_RETURN(ColumnId value, Resolve(column, agg_id, kOp));
    const auto& type = plan_.columns[value].type;
    if (type && !(type->Is<core::Id>() || type->Is<core::Uint32>() ||
                  type->Is<core::Int32>() || type->Is<core::Int64>())) {
      return base::ErrStatus(
          "%s%s: SUM needs an integer column but '%s' is not",
          Traceback(agg_id).c_str(), kOp, column.c_str());
    }
    std::string name = SpanText(p_, agg->name);
    ColumnId id = plan_.AddColumn(name, core::Int64{});
    acc.aggregates.push_back({op::TreeAccumulate::Function::kSum, value, id});
    output.push_back({std::move(name), id});
  }
  // All expressions see the input scope. Publish this stage's bindings only
  // after resolving every aggregate; duplicate names are ambiguous on lookup.
  for (NamedColumn& column : output) {
    Bind(std::move(column));
  }
  plan_.ops.emplace_back(std::move(acc));
  return base::OkStatus();
}

LogicalPlan Compiler::Finish() {
  return std::move(plan_);
}

}  // namespace

base::StatusOr<LogicalPlan> Compile(SyntaqliteParser* p,
                                    uint32_t pipeline,
                                    const NodeSourceFn& source,
                                    const Catalog& catalog) {
  const auto& n = Node<SyntaqliteNode>(p, pipeline)->perfetto_pipeline;
  Compiler compiler(p, source, catalog);
  RETURN_IF_ERROR(compiler.CompileSource(n.from));
  if (syntaqlite_node_is_present(n.stages)) {
    const auto* stages = Node<SyntaqlitePerfettoPipeStageList>(p, n.stages);
    uint32_t count = syntaqlite_list_count(stages);
    for (uint32_t i = 0; i < count; i++) {
      RETURN_IF_ERROR(
          compiler.CompileStage(syntaqlite_list_child_id(stages, i)));
    }
  }
  return compiler.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
