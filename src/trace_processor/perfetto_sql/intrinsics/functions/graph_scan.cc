/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/graph_scan.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/runtime_table.h"
#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/array.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/node.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/row_dataframe.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/value.h"
#include "src/trace_processor/sqlite/bindings/sqlite_bind.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_stmt.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace {

struct NodeState {
  uint32_t depth = 0;
  enum : uint8_t {
    kUnvisited,
    kWaitingForDescendants,
    kDone,
  } visit_state = kUnvisited;
};

struct DepthTable {
  RuntimeTable::Builder builder;
  uint32_t row_count = 0;
};

struct GraphScanner {
  base::StatusOr<std::unique_ptr<RuntimeTable>> Run();
  std::vector<uint32_t> InitializeStateFromMaxNode();
  uint32_t DfsAndComputeMaxDepth(std::vector<uint32_t> stack);
  base::Status PushDownStartingAggregates(RuntimeTable::Builder& res,
                                          uint32_t& res_row_count);
  base::StatusOr<SqliteEngine::PreparedStatement> PrepareStatement() const;
  base::Status PushDownAggregates(SqliteEngine::PreparedStatement& agg_stmt,
                                  uint32_t agg_col_count,
                                  RuntimeTable::Builder& res,
                                  uint32_t& res_row_count);

  const std::vector<uint32_t>& GetEdges(uint32_t id) {
    return id < graph.size() ? graph[id].outgoing_edges : empty_edges;
  }

  PerfettoSqlEngine* engine;
  StringPool* pool;
  const perfetto_sql::Graph& graph;
  const perfetto_sql::RowDataframe& inits;
  std::string_view reduce;
  std::vector<uint32_t> empty_edges;

  std::vector<NodeState> state;
  std::vector<DepthTable> tables_per_depth;
};

std::vector<uint32_t> GraphScanner::InitializeStateFromMaxNode() {
  std::vector<uint32_t> stack;
  auto nodes_size = static_cast<uint32_t>(graph.size());
  for (uint32_t i = 0; i < inits.size(); ++i) {
    auto start_id = static_cast<uint32_t>(
        std::get<int64_t>(inits.cells[i * inits.column_names.size()]));
    nodes_size = std::max(nodes_size, static_cast<uint32_t>(start_id) + 1);
    for (uint32_t dest : GetEdges(start_id)) {
      stack.emplace_back(static_cast<uint32_t>(dest));
    }
  }
  state = std::vector<NodeState>(nodes_size);
  return stack;
}

uint32_t GraphScanner::DfsAndComputeMaxDepth(std::vector<uint32_t> stack) {
  uint32_t max_depth = 0;
  while (!stack.empty()) {
    uint32_t source_id = stack.back();
    NodeState& source = state[source_id];
    switch (source.visit_state) {
      case NodeState::kUnvisited:
        source.visit_state = NodeState::kWaitingForDescendants;
        for (uint32_t dest_id : GetEdges(source_id)) {
          stack.push_back(dest_id);
        }
        break;
      case NodeState::kWaitingForDescendants:
        stack.pop_back();
        source.visit_state = NodeState::kDone;
        for (uint32_t dest_id : GetEdges(source_id)) {
          PERFETTO_DCHECK(state[dest_id].visit_state == NodeState::kDone);
          source.depth = std::max(state[dest_id].depth + 1, source.depth);
        }
        max_depth = std::max(max_depth, source.depth);
        break;
      case NodeState::kDone:
        stack.pop_back();
        break;
    }
  }
  return max_depth;
}

base::Status GraphScanner::PushDownAggregates(
    SqliteEngine::PreparedStatement& agg_stmt,
    uint32_t agg_col_count,
    RuntimeTable::Builder& res,
    uint32_t& res_row_count) {
  while (agg_stmt.Step()) {
    auto id =
        static_cast<uint32_t>(sqlite::column::Int64(agg_stmt.sqlite_stmt(), 0));
    res_row_count++;
    RETURN_IF_ERROR(res.AddInteger(0, id));
    for (uint32_t outgoing : GetEdges(id)) {
      auto& dt = tables_per_depth[state[outgoing].depth];
      dt.row_count++;
      RETURN_IF_ERROR(dt.builder.AddInteger(0, outgoing));
    }
    for (uint32_t i = 1; i < agg_col_count; ++i) {
      switch (sqlite::column::Type(agg_stmt.sqlite_stmt(), i)) {
        case sqlite::Type::kNull:
          RETURN_IF_ERROR(res.AddNull(i));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddNull(i));
          }
          break;
        case sqlite::Type::kInteger: {
          int64_t a = sqlite::column::Int64(agg_stmt.sqlite_stmt(), i);
          RETURN_IF_ERROR(res.AddInteger(i, a));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddInteger(i, a));
          }
          break;
        }
        case sqlite::Type::kText: {
          const char* a = sqlite::column::Text(agg_stmt.sqlite_stmt(), i);
          RETURN_IF_ERROR(res.AddText(i, a));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddText(i, a));
          }
          break;
        }
        case sqlite::Type::kFloat: {
          double a = sqlite::column::Double(agg_stmt.sqlite_stmt(), i);
          RETURN_IF_ERROR(res.AddFloat(i, a));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddFloat(i, a));
          }
          break;
        }
        case sqlite::Type::kBlob:
          return base::ErrStatus("Unsupported blob type");
      }
    }
  }
  return agg_stmt.status();
}

base::StatusOr<SqliteEngine::PreparedStatement> GraphScanner::PrepareStatement()
    const {
  std::vector<std::string> select_cols;
  std::vector<std::string> bind_cols;
  for (uint32_t i = 0; i < inits.column_names.size(); ++i) {
    select_cols.emplace_back(
        base::StackString<1024>("c%" PRIu32 " as %s", i,
                                inits.column_names[i].c_str())
            .ToStdString());
    bind_cols.emplace_back(base::StackString<1024>(
                               "__intrinsic_table_ptr_bind(c%" PRIu32 ", '%s')",
                               i, inits.column_names[i].c_str())
                               .ToStdString());
  }

  // TODO(lalitm): verify that the init aggregates line up correctly with the
  // aggregation macro.
  std::string raw_sql =
      "(SELECT $cols FROM __intrinsic_table_ptr($var) WHERE $where)";
  raw_sql = base::ReplaceAll(raw_sql, "$cols", base::Join(select_cols, ","));
  raw_sql = base::ReplaceAll(raw_sql, "$where", base::Join(bind_cols, " AND "));
  std::string res = base::ReplaceAll(std::string(reduce), "$table", raw_sql);
  return engine->PrepareSqliteStatement(
      SqlSource::FromTraceProcessorImplementation("SELECT * FROM " + res));
}

base::Status GraphScanner::PushDownStartingAggregates(
    RuntimeTable::Builder& res,
    uint32_t& res_row_count) {
  for (uint32_t i = 0; i < inits.size(); ++i) {
    const auto* cell = inits.cells.data() + i * inits.column_names.size();
    auto id = static_cast<uint32_t>(std::get<int64_t>(*cell));
    RETURN_IF_ERROR(res.AddInteger(0, id));
    res_row_count++;
    for (uint32_t outgoing : GetEdges(id)) {
      auto& dt = tables_per_depth[state[outgoing].depth];
      dt.row_count++;
      RETURN_IF_ERROR(dt.builder.AddInteger(0, outgoing));
    }
    for (uint32_t j = 1; j < inits.column_names.size(); ++j) {
      switch (cell[j].index()) {
        case perfetto_sql::ValueIndex<std::monostate>():
          RETURN_IF_ERROR(res.AddNull(j));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddNull(j));
          }
          break;
        case perfetto_sql::ValueIndex<int64_t>(): {
          int64_t r = std::get<int64_t>(cell[j]);
          RETURN_IF_ERROR(res.AddInteger(j, r));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddInteger(j, r));
          }
          break;
        }
        case perfetto_sql::ValueIndex<double>(): {
          double r = std::get<double>(cell[j]);
          RETURN_IF_ERROR(res.AddFloat(j, r));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddFloat(j, r));
          }
          break;
        }
        case perfetto_sql::ValueIndex<std::string>(): {
          const char* r = std::get<std::string>(cell[j]).c_str();
          RETURN_IF_ERROR(res.AddText(j, r));
          for (uint32_t outgoing : GetEdges(id)) {
            auto& dt = tables_per_depth[state[outgoing].depth];
            RETURN_IF_ERROR(dt.builder.AddText(j, r));
          }
          break;
        }
        default:
          PERFETTO_FATAL("Invalid index");
      }
    }
  }
  return base::OkStatus();
}

base::StatusOr<std::unique_ptr<RuntimeTable>> GraphScanner::Run() {
  if (!inits.id_column_index) {
    return base::ErrStatus(
        "GRAPH_SCAN: 'id' column is not present in initial nodes table");
  }
  if (inits.id_column_index != 0) {
    return base::ErrStatus(
        "GRAPH_SCAN: 'id' column must be the first column in the initial "
        "nodes table");
  }

  // The basic idea of this algorithm is as follows:
  // 1) Setup the state vector by figuring out the maximum id in the initial and
  //    graph tables.
  // 2) Do a DFS to compute the depth of each node and figure out the max depth.
  // 3) Setup all the table builders for each depth.
  // 4) For all the starting nodes, push down their values to their dependents
  //    and also store the aggregates in the final result table.
  // 5) Going from highest depth downward, run the aggregation SQL the user
  //    specified, push down those values to their dependents and also store the
  //    aggregates in the final result table.
  // 6) Return the final result table.
  //
  // The complexity of this algorithm is O(n) in both memory and CPU.
  //
  // TODO(lalitm): there is a significant optimization we can do here: instead
  // of pulling the data from SQL to C++ and then feeding that to the runtime
  // table builder, we could just have an aggregate function which directly
  // writes into the table itself. This would be better because:
  //   1) It would be faster
  //   2) It would remove the need for first creating a row dataframe and then a
  //      table builder for the initial nodes
  //   3) It would allow code deduplication between the initial query, the step
  //      query and also CREATE PERFETTO TABLE: the code here is very similar to
  //      the code in PerfettoSqlEngine.

  RuntimeTable::Builder res(pool, inits.column_names);
  uint32_t res_row_count = 0;
  uint32_t max_depth = DfsAndComputeMaxDepth(InitializeStateFromMaxNode());

  for (uint32_t i = 0; i < max_depth + 1; ++i) {
    tables_per_depth.emplace_back(
        DepthTable{RuntimeTable::Builder(pool, inits.column_names), 0});
  }

  RETURN_IF_ERROR(PushDownStartingAggregates(res, res_row_count));
  ASSIGN_OR_RETURN(auto agg_stmt, PrepareStatement());
  RETURN_IF_ERROR(agg_stmt.status());

  uint32_t agg_col_count = sqlite::column::Count(agg_stmt.sqlite_stmt());
  std::vector<std::string> aggregate_cols;
  aggregate_cols.reserve(agg_col_count);
  for (uint32_t i = 0; i < agg_col_count; ++i) {
    aggregate_cols.emplace_back(
        sqlite::column::Name(agg_stmt.sqlite_stmt(), i));
  }

  if (aggregate_cols != inits.column_names) {
    return base::ErrStatus(
        "graph_scan: aggregate SQL columns do not match init columns");
  }

  for (auto i = static_cast<int64_t>(tables_per_depth.size() - 1); i >= 0;
       --i) {
    int err = sqlite::stmt::Reset(agg_stmt.sqlite_stmt());
    if (err != SQLITE_OK) {
      return base::ErrStatus("Failed to reset statement");
    }
    auto idx = static_cast<uint32_t>(i);
    ASSIGN_OR_RETURN(auto depth_tab,
                     std::move(tables_per_depth[idx].builder)
                         .Build(tables_per_depth[idx].row_count));
    err = sqlite::bind::Pointer(
        agg_stmt.sqlite_stmt(), 1, depth_tab.release(), "TABLE", [](void* tab) {
          std::unique_ptr<RuntimeTable>(static_cast<RuntimeTable*>(tab));
        });
    if (err != SQLITE_OK) {
      return base::ErrStatus("Failed to bind pointer %d", err);
    }
    RETURN_IF_ERROR(
        PushDownAggregates(agg_stmt, agg_col_count, res, res_row_count));
  }
  return std::move(res).Build(res_row_count);
}

struct GraphScan : public SqliteFunction<GraphScan> {
  static constexpr char kName[] = "__intrinsic_graph_scan";
  static constexpr int kArgCount = 4;
  struct UserDataContext {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);

    auto* user_data = GetUserData(ctx);
    const char* reduce = sqlite::value::Text(argv[2]);
    if (!reduce) {
      return sqlite::result::Error(ctx,
                                   "graph_scan: aggegate SQL cannot be null");
    }
    const char* column_list = sqlite::value::Text(argv[3]);
    if (!column_list) {
      return sqlite::result::Error(ctx,
                                   "graph_scan: column list cannot be null");
    }

    std::vector<std::string> col_names{"id"};
    for (const auto& c :
         base::SplitString(base::StripChars(column_list, "()", ' '), ",")) {
      col_names.push_back(base::TrimWhitespace(c));
    }

    const auto* init = sqlite::value::Pointer<perfetto_sql::RowDataframe>(
        argv[1], "ROW_DATAFRAME");
    if (!init) {
      SQLITE_ASSIGN_OR_RETURN(
          ctx, auto table,
          RuntimeTable::Builder(user_data->pool, std::move(col_names))
              .Build(0));
      return sqlite::result::UniquePointer(ctx, std::move(table), "TABLE");
    }
    if (col_names != init->column_names) {
      return sqlite::result::Error(
          ctx, "graph_scan: column list does not match initial table list");
    }

    const auto* nodes =
        sqlite::value::Pointer<perfetto_sql::Graph>(argv[0], "GRAPH");
    GraphScanner scanner{
        user_data->engine,
        user_data->pool,
        nodes ? *nodes : perfetto_sql::Graph(),
        *init,
        reduce,
        {},
        {},
        {},
    };
    auto result = scanner.Run();
    if (!result.ok()) {
      return sqlite::utils::SetError(ctx, result.status());
    }
    return sqlite::result::UniquePointer(ctx, std::move(*result), "TABLE");
  }
};

}  // namespace

base::Status RegisterGraphScanFunctions(PerfettoSqlEngine& engine,
                                        StringPool* pool) {
  return engine.RegisterSqliteFunction<GraphScan>(
      std::make_unique<GraphScan::UserDataContext>(
          GraphScan::UserDataContext{&engine, pool}));
}

}  // namespace perfetto::trace_processor
