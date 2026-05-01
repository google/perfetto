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

// One entry in the wakeup graph. Mirrors the columns of the
// `_wakeup_graph` stdlib table: each entry represents a single transition
// of a thread from idle to runnable, plus its run that follows.
struct WakeupNode {
  uint32_t utid = 0;
  // [ts, ts + dur] is when the thread was running. [ts - idle_dur, ts] is
  // the idle period that immediately preceded this run; if `idle_dur` is
  // unset (e.g. because this is the first recorded entry on the thread),
  // the idle period is treated as extending back as far as the caller's
  // recursion window allows — the chain still propagates through the
  // waker, just without a hard lower bound from this node.
  int64_t ts = 0;
  int64_t dur = 0;
  std::optional<int64_t> idle_dur;
  // Id of the wakeup-graph entry on the thread that woke this one. Null if
  // the run starts on a thread with no recorded waker.
  std::optional<uint32_t> waker_id;
  // Id of this thread's previous wakeup-graph entry (the run that ended
  // when this idle period started). Null for the first entry on a thread.
  std::optional<uint32_t> prev_id;
  // Whether this wakeup was a self-wake (e.g. IRQ). When set, the userspace
  // critical-path semantics chain through `prev_id` rather than `waker_id`.
  bool is_idle_reason_self = false;
};

// Pointer-tagged value that the `__intrinsic_critical_path_walk` function
// consumes. Dense vector indexed by node id; positions for ids that were
// never inserted contain `std::nullopt`. The constructor of the agg fills
// any gaps so this remains O(1) lookup at the cost of one bool per id.
struct WakeupGraph {
  static constexpr char kName[] = "WAKEUP_GRAPH";

  // Indexed by node id. Sparse over the id range, dense in practice for
  // wakeup_graph since ids are derived from thread_state ids.
  std::vector<std::optional<WakeupNode>> nodes_by_id;
};

}  // namespace perfetto::trace_processor::perfetto_sql

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TYPES_WAKEUP_GRAPH_H_
