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

#include "src/trace_processor/plugins/dominator_tree/dominator_tree.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/dominator_tree/tables_py.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/graph.h"

namespace perfetto::trace_processor {

namespace {

struct AggCtx : sqlite::AggregateContext<AggCtx> {
  std::vector<uint32_t> sources;
  std::vector<uint32_t> targets;
  uint32_t node_count = 0;
  std::optional<uint32_t> start_id;
};

}  // namespace

void DominatorTree::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != kArgCount) {
    return sqlite::result::Error(
        ctx, "dominator_tree: incorrect number of arguments");
  }
  int64_t raw_source = sqlite3_value_int64(argv[0]);
  int64_t raw_dest = sqlite3_value_int64(argv[1]);
  int64_t raw_start = sqlite3_value_int64(argv[2]);
  if (raw_source < 0 || raw_source > std::numeric_limits<int32_t>::max() ||
      raw_dest < 0 || raw_dest > std::numeric_limits<int32_t>::max() ||
      raw_start < 0 || raw_start > std::numeric_limits<int32_t>::max()) {
    return sqlite::result::Error(
        ctx, "dominator_tree: node ids must be non-negative 32-bit integers");
  }

  auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
  auto source = static_cast<uint32_t>(raw_source);
  auto dest = static_cast<uint32_t>(raw_dest);
  agg_ctx.sources.push_back(source);
  agg_ctx.targets.push_back(dest);
  agg_ctx.node_count = std::max(agg_ctx.node_count, std::max(source, dest) + 1);
  if (PERFETTO_UNLIKELY(!agg_ctx.start_id)) {
    agg_ctx.start_id = static_cast<uint32_t>(raw_start);
  }
}

void DominatorTree::Final(sqlite3_context* ctx) {
  auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
  auto table = std::make_unique<tables::DominatorTreeTable>(GetUserData(ctx));
  if (auto* agg_ctx = raw_agg_ctx.get(); agg_ctx) {
    uint32_t start_id = *agg_ctx->start_id;
    if (start_id >= agg_ctx->node_count) {
      return sqlite::result::Error(
          ctx, "dominator_tree: root node is not in the graph");
    }
    util::GraphTree tree = util::DominatorTree(
        util::Digraph::FromEdges(agg_ctx->node_count, agg_ctx->sources,
                                 agg_ctx->targets),
        start_id);
    if (tree.order.size() <= 1) {
      return sqlite::result::Error(
          ctx,
          "dominator_tree: non empty graph must contain root and another node");
    }
    for (uint32_t node : tree.order) {
      tables::DominatorTreeTable::Row r;
      r.node_id = node;
      r.dominator_node_id = node == start_id
                                ? std::nullopt
                                : std::make_optional(tree.parent[node]);
      table->Insert(r);
    }
  }
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<dataframe::Dataframe>(std::move(table->dataframe())),
      "TABLE");
}

namespace dominator_tree {
namespace {

class DominatorTreePlugin : public Plugin<DominatorTreePlugin> {
 public:
  ~DominatorTreePlugin() override;

  void RegisterAggregateFunctions(
      PerfettoSqlConnection*,
      std::vector<AggregateFunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeAggregateRegistration<DominatorTree>(pool));
  }
};

DominatorTreePlugin::~DominatorTreePlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<DominatorTreePlugin>();
      },
      DominatorTreePlugin::kPluginId, DominatorTreePlugin::kDepIds.data(),
      DominatorTreePlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace dominator_tree
}  // namespace perfetto::trace_processor
