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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_FLAMECHART_FLAMECHART_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_FLAMECHART_FLAMECHART_H_

#include <cstdint>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::flamechart {

// Computes the flame-chart rectangle set from a stack tree and a series of
// sample points.
//
// The tree encodes the stack structure: `tree.parent` links each node to its
// caller (row indices, kNullParent for roots, parents before children). Each
// point is a (ts[i], leaf_id[i]) pair: a sample timestamp and the tree node
// of the innermost (leaf) frame at that time. |ts| must be non-decreasing and
// the two spans must have equal size. Leaf ids are original node ids (e.g. a
// callsite id), resolved through the id -> row index persisted on the tree by
// core::BuildTree; for trees without an id column the ids are row indices.
// Unresolvable leaves are skipped: open segments are kept as-is and the gap
// is simply not attributed to any frame.
//
// The returned dataframe has five columns, in this order:
//   0: ts           - segment start timestamp
//   1: dur          - segment duration
//   2: depth        - stack depth (0 = outermost/root frame)
//   3: id           - original node id of the segment's frame (the tree's id
//      column), so runs join directly against the tree's source table; row
//      index for trees without an id column
//   4: sample_count - number of points in the segment
//
// The output is the maximal-run (prefix-merge) decomposition: for each depth,
// a segment spans the time range over which the same frame was continuously
// present at that depth. Consecutive points sharing a stack prefix extend the
// shared segments; only divergent depths open new segments. This keeps the
// output far below (points x depth): the leaf depth yields about one segment
// per run while shallow depths yield only a handful. Segments still open
// after the last point are closed at its timestamp, so a run seen in a single
// point has zero duration.
//
// The sweep is a single streaming pass over the points: O(1) per point when
// the leaf is unchanged (the full stack is identical), otherwise the walk
// cost is proportional to the number of divergent depths. Memory is O(open
// segments) plus the emitted output.
base::StatusOr<dataframe::Dataframe> Build(const core::Tree& tree,
                                           core::Span<const int64_t> ts,
                                           core::Span<const int64_t> leaf_id,
                                           StringPool* pool);

}  // namespace perfetto::trace_processor::flamechart

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_FLAMECHART_FLAMECHART_H_
