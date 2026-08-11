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

#include "src/trace_processor/plugins/flamechart/flamechart_function.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/flamechart/flamechart.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {

constexpr char kTreePointerType[] = "TREE";
constexpr char kTablePointerType[] = "TABLE";

// Aggregate computing the flame-chart runs for a stack tree and a series of
// sample points.
//
// Args, per aggregated row:
//   0: TREE pointer - the stack tree, from __intrinsic_tree_from_table. Must
//      be the same tree for every row.
//   1: ts           - sample timestamp (integer). Must be non-decreasing in
//      aggregation order.
//   2: leaf_id      - tree node id of the innermost (leaf) frame (integer).
//
// Rows with a null ts or leaf_id are skipped. Returns a
// `dataframe::Dataframe*` tagged "TABLE" with columns (ts, dur, depth, id,
// sample_count), consumed via `__intrinsic_table_ptr`; `id` is the frame's
// node id in the tree's source table.
struct FlamechartAgg : public sqlite::AggregateFunction<FlamechartAgg> {
  static constexpr char kName[] = "__intrinsic_flamechart";
  static constexpr int kArgCount = 3;
  using UserData = StringPool;

  struct AggCtx : sqlite::AggregateContext<AggCtx> {
    const core::Tree* tree = nullptr;
    std::vector<int64_t> ts;
    std::vector<int64_t> leaf_id;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);
    auto& agg = AggCtx::GetOrCreateContextForStep(ctx);
    if (!agg.tree) {
      agg.tree = sqlite::value::Pointer<core::Tree>(argv[0], kTreePointerType);
      if (!agg.tree) {
        return sqlite::result::Error(
            ctx, "flamechart: first argument must be a TREE pointer");
      }
    }
    const auto ts_type = sqlite::value::Type(argv[1]);
    const auto leaf_type = sqlite::value::Type(argv[2]);
    if (ts_type == sqlite::Type::kNull || leaf_type == sqlite::Type::kNull) {
      return;
    }
    if (ts_type != sqlite::Type::kInteger ||
        leaf_type != sqlite::Type::kInteger) {
      return sqlite::result::Error(
          ctx, "flamechart: ts and leaf_id must be integers");
    }
    agg.ts.push_back(sqlite::value::Int64(argv[1]));
    agg.leaf_id.push_back(sqlite::value::Int64(argv[2]));
  }

  static void Final(sqlite3_context* ctx) {
    StringPool* pool = GetUserData(ctx);
    auto raw_agg = AggCtx::GetContextOrNullForFinal(ctx);
    // Zero rows: build over nothing, producing an empty runs table with the
    // correct schema.
    const core::Tree empty_tree;
    const core::Tree& tree = raw_agg ? *raw_agg.get()->tree : empty_tree;
    const core::Span<const int64_t> ts = raw_agg
                                             ? core::MakeSpan(raw_agg.get()->ts)
                                             : core::Span<const int64_t>();
    const core::Span<const int64_t> leaf_id =
        raw_agg ? core::MakeSpan(raw_agg.get()->leaf_id)
                : core::Span<const int64_t>();
    SQLITE_ASSIGN_OR_RETURN(ctx, auto runs,
                            flamechart::Build(tree, ts, leaf_id, pool));
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(runs)),
        kTablePointerType);
  }
};

}  // namespace

namespace flamechart {
namespace {

class FlamechartPlugin : public Plugin<FlamechartPlugin> {
 public:
  ~FlamechartPlugin() override;

  void RegisterAggregateFunctions(
      PerfettoSqlConnection*,
      std::vector<AggregateFunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeAggregateRegistration<FlamechartAgg>(pool));
  }
};

FlamechartPlugin::~FlamechartPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration registration(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<FlamechartPlugin>();
      },
      FlamechartPlugin::kPluginId, FlamechartPlugin::kDepIds.data(),
      FlamechartPlugin::kDepIds.size());
  base::ignore_result(registration);
}

}  // namespace flamechart
}  // namespace perfetto::trace_processor
