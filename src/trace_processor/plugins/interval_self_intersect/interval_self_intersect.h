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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_INTERVAL_SELF_INTERSECT_INTERVAL_SELF_INTERSECT_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_INTERVAL_SELF_INTERSECT_INTERVAL_SELF_INTERSECT_H_

namespace perfetto::trace_processor::interval_self_intersect {

// Registers the __intrinsic_interval_self_intersect SQL function: an O(n log n)
// sweep-line implementation that, for every atomic time segment defined by the
// endpoints of the input intervals and for every distinct partition tuple
// active in that segment, emits one row carrying (ts, dur, group_id, count,
// partition_cols...).
//
// Drop-in faster replacement for the SQL-stdlib `interval_self_intersect!`
// macro that routes through the general two-table `interval_intersect`.
// Algorithm inspired by dev/zezeozue/self_intersect; this build narrows the
// scope to COUNT (active intervals per partition per segment) — sum/min/max/avg
// can be layered on later if a caller needs them.
void RegisterPlugin();

}  // namespace perfetto::trace_processor::interval_self_intersect

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_INTERVAL_SELF_INTERSECT_INTERVAL_SELF_INTERSECT_H_
