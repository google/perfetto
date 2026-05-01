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

// Implementation of the SQLite functions used by the
// `sched.thread_executing_span` stdlib module to compute the critical
// path of a thread:
//
//   __intrinsic_wakeup_graph_agg
//       SQL aggregate. Consumes rows of `_wakeup_graph` and produces an
//       opaque `WakeupGraph*` pointer (tagged `WakeupGraph::kName`).
//
//   __intrinsic_critical_path_walk
//       SQL function. Takes a `WakeupGraph*`, an `IntArray*` of root ids
//       and an optional `mode` (0 = userspace, 1 = kernel; defaults to
//       userspace), and returns a `Dataframe*` (tagged "TABLE") with
//       columns (root_id, depth, ts, dur, blocker_id, blocker_utid).
//
// The walk is iterative DFS over each root's wakeup chain, with each
// frame bounded by its own [ts - idle_dur, ts + dur] window.
namespace perfetto::trace_processor {
namespace {

using perfetto_sql::WakeupGraph;
using perfetto_sql::WakeupNode;

// Aggregate that builds a `WakeupGraph` from rows of the `_wakeup_graph`
// stdlib table. Args, in order:
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

// One unit of work for the iterative walk: attribute time during
// `[window_start, window_end)` using node `node_id`, at chain `depth`
// relative to the current root.
struct Frame {
  uint32_t node_id;
  int64_t window_start;
  int64_t window_end;
  uint32_t depth;
};

// Edge-following mode for the walk. Mirrors the `_wakeup_userspace_edges`
// and `_wakeup_kernel_edges` SQL views.
enum class Mode : uint8_t {
  // IRQ self-wakes (`is_idle_reason_self=1`) chain through `prev_id`
  // (same thread); all other wakeups chain through `waker_id`.
  kUserspace = 0,
  // Always chain through `waker_id`; `is_idle_reason_self` is ignored.
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

  // Reuse the caller's stack capacity across roots; clear contents only.
  stack.clear();

  // Seed with the root's full attribution window: the idle period that
  // preceded the root's run plus the root's own run. Unknown `idle_dur`
  // collapses the idle half (no lower bound is available).
  //
  // Termination relies on the wakeup graph's causal ordering: any node
  // reachable from `root_id` via `waker_id` or `prev_id` ran strictly
  // before the current node, so each push descends to a node with a
  // smaller `ts`. The walk therefore terminates after at most one
  // (node, sub-window) pair per reachable causal predecessor.
  int64_t initial_start = root.ts - root.idle_dur.value_or(0);
  int64_t initial_end = root.ts + root.dur;
  stack.push_back({root_id, initial_start, initial_end, 0});

  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();

    if (f.window_start >= f.window_end ||
        f.node_id >= graph.nodes_by_id.size() ||
        !graph.nodes_by_id[f.node_id]) {
      continue;
    }

    const WakeupNode& n = *graph.nodes_by_id[f.node_id];
    // An unset `idle_dur` means the idle half is open below; clip it
    // by the caller's window so chain propagation through `waker_id`
    // still works without a hard lower bound from this node.
    int64_t node_idle_start =
        n.idle_dur.has_value() ? (n.ts - *n.idle_dur) : f.window_start;
    int64_t node_run_end = n.ts + n.dur;

    int64_t eff_start = std::max(f.window_start, node_idle_start);
    int64_t eff_end = std::min(f.window_end, node_run_end);

    // Caller's window predates this node's idle: descend into `prev_id`
    // at the same depth (same thread, earlier run) so the chain can
    // continue covering time before this node existed.
    if (n.idle_dur.has_value() && f.window_start < node_idle_start &&
        n.prev_id) {
      int64_t prev_window_end = std::min(f.window_end, node_idle_start);
      stack.push_back({*n.prev_id, f.window_start, prev_window_end, f.depth});
    }

    if (eff_start >= eff_end) {
      continue;
    }

    // Idle portion of the effective window: this thread was sleeping,
    // so attribute the time to whoever woke it. Userspace IRQ self-wakes
    // chain into `prev_id` at the same depth; everything else chains
    // into `waker_id` at depth + 1.
    int64_t idle_clip_start = eff_start;
    int64_t idle_clip_end = std::min(eff_end, n.ts);
    if (idle_clip_start < idle_clip_end) {
      if (mode == Mode::kUserspace && n.is_idle_reason_self) {
        if (n.prev_id) {
          // Emit a placeholder row covering the self-idle window
          // attributed to `prev_id`. The descent into `prev_id` may not
          // overlap (prev's run can end before the idle window starts),
          // so without this row the gap survives all the way through
          // `_intervals_flatten` and shows up as a missing slot in the
          // critical-path-lite UI. `_critical_path_userspace_adjusted`'s
          // `is_next_idle_reason_self → next_id` rewrite turns this id
          // into the current node's id, which is what the kernel-pass
          // join in `_critical_path_kernel_adjusted` matches against.
          if (const auto& prev = graph.nodes_by_id[*n.prev_id]) {
            tables::CriticalPathWalkTable::Row row;
            row.root_id = root_id;
            row.depth = f.depth;
            row.ts = idle_clip_start;
            row.dur = idle_clip_end - idle_clip_start;
            row.blocker_id = *n.prev_id;
            row.blocker_utid = prev->utid;
            out.Insert(row);
          }
          stack.push_back(
              {*n.prev_id, idle_clip_start, idle_clip_end, f.depth});
        }
      } else if (n.waker_id) {
        stack.push_back(
            {*n.waker_id, idle_clip_start, idle_clip_end, f.depth + 1});
      }
    }

    // Running portion of the effective window: this thread is on-CPU
    // and is therefore the blocker at this depth.
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

// Returns a `Dataframe*` (tagged "TABLE") that callers consume via
// `__intrinsic_table_ptr(...)`. Args:
//   argv[0]: WakeupGraph*  from `__intrinsic_wakeup_graph_agg`.
//   argv[1]: IntArray*     of root ids, from `__intrinsic_array_agg`.
//   argv[2]: int (optional) walk mode: 0 = userspace, 1 = kernel.
//                  NULL or omitted is treated as userspace.
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
