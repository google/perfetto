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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/dfs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/tables_py.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"

namespace perfetto::trace_processor {
namespace tables {
DfsTable::~DfsTable() = default;
}  // namespace tables

namespace {

using Destinations = std::vector<uint32_t>;

struct AggCtx : SqliteAggregateContext<AggCtx> {
  std::vector<Destinations> source_to_destinations_map;
  std::optional<uint32_t> start_id;
};

}  // namespace

void Dfs::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != kArgCount) {
    return sqlite::result::Error(ctx, "dfs: incorrect number of arguments");
  }

  auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
  auto source = static_cast<uint32_t>(sqlite3_value_int64(argv[0]));
  auto dest = static_cast<uint32_t>(sqlite3_value_int64(argv[1]));

  // For every source node, create a mapping to the destination nodes.
  agg_ctx.source_to_destinations_map.resize(
      std::max<size_t>(agg_ctx.source_to_destinations_map.size(),
                       std::max(source + 1, dest + 1)));
  agg_ctx.source_to_destinations_map[source].push_back(dest);
  if (PERFETTO_UNLIKELY(!agg_ctx.start_id)) {
    agg_ctx.start_id = static_cast<uint32_t>(sqlite3_value_int64(argv[2]));
  }
}

void Dfs::Final(sqlite3_context* ctx) {
  auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
  auto table = std::make_unique<tables::DfsTable>(GetUserData(ctx));
  if (auto* agg_ctx = raw_agg_ctx.get(); agg_ctx) {
    std::vector<uint8_t> seen(agg_ctx->source_to_destinations_map.size());
    struct StackState {
      uint32_t id;
      std::optional<uint32_t> parent_id;
    };

    std::vector<StackState> stack{{*agg_ctx->start_id, std::nullopt}};
    while (!stack.empty()) {
      StackState state = stack.back();
      stack.pop_back();

      if (seen[state.id]) {
        continue;
      }
      seen[state.id] = true;

      tables::DfsTable::Row row;
      row.node_id = state.id;
      row.parent_node_id = state.parent_id;
      table->Insert(row);

      PERFETTO_DCHECK(state.id < agg_ctx->source_to_destinations_map.size());
      const auto& children = agg_ctx->source_to_destinations_map[state.id];
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        stack.emplace_back(StackState{*it, state.id});
      }
    }
  }
  return sqlite::result::RawPointer(
      ctx, table.release(), "TABLE",
      [](void* ptr) { delete static_cast<tables::DfsTable*>(ptr); });
}

}  // namespace perfetto::trace_processor
