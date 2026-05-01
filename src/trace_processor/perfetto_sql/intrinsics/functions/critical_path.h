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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_CRITICAL_PATH_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_CRITICAL_PATH_H_

#include "perfetto/base/status.h"

namespace perfetto::trace_processor {

class PerfettoSqlEngine;
class StringPool;

// Registers:
//   - __intrinsic_wakeup_graph_agg: aggregate that builds a WakeupGraph
//       pointer from per-row inputs (id, utid, ts, dur, idle_dur, waker_id,
//       prev_id, is_idle_reason_self).
//   - __intrinsic_critical_path_walk: function that takes
//       (wakeup_graph_ptr, root_id_array_ptr [, mode]) and returns a
//       Dataframe pointer with columns
//       (root_id, depth, ts, dur, blocker_id, blocker_utid).
//       `mode` is 0 for userspace edges (default; IRQ self-wakes chain
//       through `prev_id`) or 1 for kernel edges (always chains through
//       `waker_id`).
base::Status RegisterCriticalPathFunctions(PerfettoSqlEngine& engine,
                                           StringPool& pool);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_CRITICAL_PATH_H_
