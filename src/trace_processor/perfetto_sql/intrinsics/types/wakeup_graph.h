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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TYPES_WAKEUP_GRAPH_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TYPES_WAKEUP_GRAPH_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace perfetto::trace_processor::perfetto_sql {

// One entry in the wakeup graph: a single transition of a thread from
// idle to runnable, plus the run that follows. Mirrors the columns of
// the `_wakeup_graph` stdlib table.
struct WakeupNode {
  uint32_t utid = 0;
  // The thread is running across [ts, ts + dur). The preceding idle
  // period is [ts - *idle_dur, ts) when `idle_dur` is set; when unset,
  // the idle period has no lower bound from this node and is clipped
  // by the caller's recursion window instead.
  int64_t ts = 0;
  int64_t dur = 0;
  std::optional<int64_t> idle_dur;
  // Id of the wakeup-graph entry on the thread that woke this one.
  // Empty when this thread's run starts with no recorded waker.
  std::optional<uint32_t> waker_id;
  // Id of this thread's previous wakeup-graph entry (the run that ended
  // when this idle period started). Empty for the first entry on a
  // thread.
  std::optional<uint32_t> prev_id;
  // True when the wakeup is a self-wake (e.g. IRQ). Userspace
  // critical-path semantics chain through `prev_id` rather than
  // `waker_id` for such entries.
  bool is_idle_reason_self = false;
};

// Pointer-tagged value consumed by `__intrinsic_critical_path_walk`.
// Indexed by node id; gaps in the id space hold default-constructed
// `std::nullopt`. Direct array indexing keeps lookup O(1); the per-slot
// `sizeof(optional<WakeupNode>)` overhead is acceptable because
// wakeup-graph ids derive from thread_state ids and are dense in
// practice.
struct WakeupGraph {
  static constexpr char kName[] = "WAKEUP_GRAPH";

  std::vector<std::optional<WakeupNode>> nodes_by_id;
};

}  // namespace perfetto::trace_processor::perfetto_sql

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TYPES_WAKEUP_GRAPH_H_
