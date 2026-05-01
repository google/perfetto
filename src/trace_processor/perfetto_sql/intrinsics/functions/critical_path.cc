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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/critical_path.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/tables_py.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/array.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/wakeup_graph.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {
namespace {

using perfetto_sql::WakeupGraph;
using perfetto_sql::WakeupNode;

// Per-root iteration cap. Termination of the walk is guaranteed
// structurally: every recursive step strictly reduces the window's
// upper bound (the waker frame is bounded above by the child's `ts`,
// the prev frame is bounded above by `node_idle_start`), and time
// monotonically decreases as we descend either edge. This cap is
// purely a safety belt against degenerate inputs that could in theory
// fan out across many overlapping windows; the value is far above
// anything observed in real traces.
constexpr uint32_t kMaxIterationsPerRoot = 1u << 20;  // 1,048,576

// Aggregate that materialises a WakeupGraph from rows of the
// `_wakeup_graph` stdlib table. One Step() per row.
//
// Args (8, in order):
//   id, utid, ts, dur, idle_dur, waker_id, prev_id, is_idle_reason_self
struct WakeupGraphAgg : public sqlite::AggregateFunction<WakeupGraphAgg> {
  static constexpr char kName[] = "__intrinsic_wakeup_graph_agg";
  static constexpr int kArgCount = 8;

  struct AggCtx : sqlite::AggregateContext<AggCtx> {
    WakeupGraph graph;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);
    auto& g = AggCtx::GetOrCreateContextForStep(ctx).graph;

    auto id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
    if (id >= g.nodes_by_id.size()) {
      g.nodes_by_id.resize(id + 1);
    }
    WakeupNode n;
    n.utid = static_cast<uint32_t>(sqlite::value::Int64(argv[1]));
    n.ts = sqlite::value::Int64(argv[2]);
    n.dur = sqlite::value::Int64(argv[3]);
    if (sqlite::value::Type(argv[4]) != sqlite::Type::kNull) {
      n.idle_dur = sqlite::value::Int64(argv[4]);
    }
    if (sqlite::value::Type(argv[5]) != sqlite::Type::kNull) {
      n.waker_id = static_cast<uint32_t>(sqlite::value::Int64(argv[5]));
    }
    if (sqlite::value::Type(argv[6]) != sqlite::Type::kNull) {
      n.prev_id = static_cast<uint32_t>(sqlite::value::Int64(argv[6]));
    }
    n.is_idle_reason_self = sqlite::value::Int64(argv[7]) != 0;
    g.nodes_by_id[id] = std::move(n);
  }

  static void Final(sqlite3_context* ctx) {
    auto raw = AggCtx::GetContextOrNullForFinal(ctx);
    if (!raw.get()) {
      return sqlite::result::Null(ctx);
    }
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<WakeupGraph>(std::move(raw.get()->graph)),
        WakeupGraph::kName);
  }
};

// Per-frame work item used by the iterative walk. Represents "we should
// attribute time during [window_start, window_end) using node `node_id`'s
// own structure, at depth `depth` in the chain from the current root."
struct Frame {
  uint32_t node_id;
  int64_t window_start;
  int64_t window_end;
  uint32_t depth;
};

enum class Mode : uint8_t {
  // Userspace: IRQ self-wakes (`is_idle_reason_self=1`) chain through
  // `prev_id` (same thread) instead of `waker_id`. Matches the existing
  // `_wakeup_userspace_edges` view.
  kUserspace = 0,
  // Kernel: always chain through `waker_id`, ignoring
  // `is_idle_reason_self`. Matches `_wakeup_kernel_edges`.
  kKernel = 1,
};

void WalkOneRoot(const WakeupGraph& graph,
                 uint32_t root_id,
                 Mode mode,
                 tables::CriticalPathWalkTable& out,
                 std::vector<Frame>& stack) {
  if (root_id >= graph.nodes_by_id.size() || !graph.nodes_by_id[root_id]) {
    return;
  }
  const WakeupNode& root = *graph.nodes_by_id[root_id];

  // Reset per-root scratch storage. Keep capacity to avoid reallocs.
  stack.clear();

  // Initial window: the idle period that preceded the root run, plus the
  // root's own running interval. The idle portion gets attributed via the
  // waker chain; the running portion is the root running on the CPU.
  // If `idle_dur` is unknown for the root, fall back to a 0-length idle —
  // we have no way to bound the search backward in that case.
  int64_t initial_start = root.ts - root.idle_dur.value_or(0);
  int64_t initial_end = root.ts + root.dur;
  stack.push_back({root_id, initial_start, initial_end, 0});

  uint32_t iterations = 0;
  while (!stack.empty()) {
    if (++iterations > kMaxIterationsPerRoot) {
      break;
    }
    Frame f = stack.back();
    stack.pop_back();

    if (f.window_start >= f.window_end ||
        f.node_id >= graph.nodes_by_id.size() ||
        !graph.nodes_by_id[f.node_id]) {
      continue;
    }

    const WakeupNode& n = *graph.nodes_by_id[f.node_id];
    // If `idle_dur` is unset, the prior idle is open-ended: clip purely
    // by the caller's window so the chain can still propagate through
    // `waker_id` without a hard lower bound from this node.
    int64_t node_idle_start =
        n.idle_dur.has_value() ? (n.ts - *n.idle_dur) : f.window_start;
    int64_t node_run_end = n.ts + n.dur;

    int64_t eff_start = std::max(f.window_start, node_idle_start);
    int64_t eff_end = std::min(f.window_end, node_run_end);

    // Time before this node's own idle window started: this thread was
    // running on its prior wakeup-graph entry. Recurse into prev_id at
    // the same depth (same thread, just an earlier run). Without this
    // step the chain dead-ends as soon as the immediate waker's run
    // doesn't span the full caller window — the bulk of the runaway-
    // looking "uncovered tail" symptom we see today.
    if (n.idle_dur.has_value() && f.window_start < node_idle_start &&
        n.prev_id) {
      int64_t prev_window_end = std::min(f.window_end, node_idle_start);
      stack.push_back({*n.prev_id, f.window_start, prev_window_end, f.depth});
    }

    if (eff_start >= eff_end) {
      continue;
    }

    // Idle portion: attribute time during which this thread was sleeping
    // by chaining into the waker (cross-thread) or prev_id (IRQ self-wake,
    // matching the existing _wakeup_userspace_edges semantic).
    int64_t idle_clip_start = eff_start;
    int64_t idle_clip_end = std::min(eff_end, n.ts);
    if (idle_clip_start < idle_clip_end) {
      if (mode == Mode::kUserspace && n.is_idle_reason_self) {
        if (n.prev_id) {
          stack.push_back(
              {*n.prev_id, idle_clip_start, idle_clip_end, f.depth});
        }
      } else if (n.waker_id) {
        stack.push_back(
            {*n.waker_id, idle_clip_start, idle_clip_end, f.depth + 1});
      }
    }

    // Running portion: this thread is on-CPU, so it is the blocker at
    // this depth.
    int64_t run_start = std::max(eff_start, n.ts);
    if (run_start < eff_end) {
      tables::CriticalPathWalkTable::Row row;
      row.root_id = root_id;
      row.depth = f.depth;
      row.ts = run_start;
      row.dur = eff_end - run_start;
      row.blocker_id = f.node_id;
      row.blocker_utid = n.utid;
      out.Insert(row);
    }
  }
}

// Function consumed via __intrinsic_table_ptr(...). Args:
//   argv[0]: WakeupGraph*  (from __intrinsic_wakeup_graph_agg)
//   argv[1]: IntArray*     (from __intrinsic_array_agg of root ids)
//   argv[2]: int  mode (0=userspace, 1=kernel). Defaults to userspace
//                  if NULL or omitted.
struct CriticalPathWalk : public sqlite::AggregateFunction<CriticalPathWalk> {
  static constexpr char kName[] = "__intrinsic_critical_path_walk";
  static constexpr int kArgCount = -1;
  using UserData = StringPool;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc < 2 || argc > 3) {
      return sqlite::result::Error(
          ctx,
          "__intrinsic_critical_path_walk: expected (graph, root_ids "
          "[, mode])");
    }
    auto out =
        std::make_unique<tables::CriticalPathWalkTable>(GetUserData(ctx));

    auto* graph =
        sqlite::value::Pointer<WakeupGraph>(argv[0], WakeupGraph::kName);
    auto* roots =
        sqlite::value::Pointer<perfetto_sql::IntArray>(argv[1], "ARRAY<LONG>");
    if (!graph || !roots || graph->nodes_by_id.empty() || roots->empty()) {
      return sqlite::result::UniquePointer(
          ctx,
          std::make_unique<dataframe::Dataframe>(std::move(out->dataframe())),
          "TABLE");
    }

    Mode mode = Mode::kUserspace;
    if (argc == 3 && sqlite::value::Type(argv[2]) != sqlite::Type::kNull) {
      int64_t v = sqlite::value::Int64(argv[2]);
      if (v == 1) {
        mode = Mode::kKernel;
      } else if (v != 0) {
        return sqlite::result::Error(
            ctx,
            "__intrinsic_critical_path_walk: mode must be 0 (userspace) or "
            "1 (kernel)");
      }
    }

    std::vector<Frame> stack;
    for (int64_t raw_root : *roots) {
      auto root_id = static_cast<uint32_t>(raw_root);
      WalkOneRoot(*graph, root_id, mode, *out, stack);
    }
    return sqlite::result::UniquePointer(
        ctx,
        std::make_unique<dataframe::Dataframe>(std::move(out->dataframe())),
        "TABLE");
  }
};

}  // namespace

base::Status RegisterCriticalPathFunctions(PerfettoSqlEngine& engine,
                                           StringPool& pool) {
  RETURN_IF_ERROR(engine.RegisterAggregateFunction<WakeupGraphAgg>(nullptr));
  return engine.RegisterFunction<CriticalPathWalk>(&pool);
}

}  // namespace perfetto::trace_processor
