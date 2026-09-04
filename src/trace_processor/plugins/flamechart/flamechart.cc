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

#include "src/trace_processor/plugins/flamechart/flamechart.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::flamechart {
namespace {

// One open (unclosed) segment per stack depth during the sweep.
struct OpenSegment {
  // Tree row index of the frame at this depth.
  uint32_t row;
  // Timestamp at which the segment opened.
  int64_t start;
  // Number of points accumulated in this segment so far.
  int64_t count;
};

}  // namespace

base::StatusOr<dataframe::Dataframe> Build(const core::Tree& tree,
                                           core::Span<const int64_t> ts,
                                           core::Span<const int64_t> leaf_id,
                                           StringPool* pool) {
  PERFETTO_CHECK(ts.size() == leaf_id.size());

  // Output ids are the tree's original node ids so consumers can join the
  // runs back against the table the tree was built from. Trees without an
  // Int64 id column (built by hand) fall back to row indices, matching how
  // their leaf ids resolve.
  const int64_t* original_ids =
      !tree.columns.empty() && tree.columns[0].type.Is<core::Int64>()
          ? tree.columns[0].unchecked_data<int64_t>()
          : nullptr;

  // Segments are streamed straight into the output dataframe: memory stays
  // O(open segments) plus the output.
  dataframe::AdhocDataframeBuilder builder(
      {"ts", "dur", "depth", "id", "sample_count"}, pool,
      dataframe::AdhocDataframeBuilder::Options{
          {}, dataframe::NullabilityType::kDenseNull, /*emit_auto_id=*/false});
  const auto emit = [&](const OpenSegment& seg, size_t depth, int64_t end) {
    builder.PushNonNull(0, seg.start);
    builder.PushNonNull(1, end - seg.start);
    builder.PushNonNull(2, static_cast<int64_t>(depth));
    builder.PushNonNull(3, original_ids ? original_ids[seg.row]
                                        : static_cast<int64_t>(seg.row));
    builder.PushNonNull(4, seg.count);
  };

  std::vector<OpenSegment> open;
  // Stack path of the current point, innermost frame first.
  std::vector<uint32_t> path;

  int64_t last_ts = 0;
  for (size_t i = 0; i < ts.size(); ++i) {
    if (i > 0 && ts[i] < last_ts) {
      return base::ErrStatus("flamechart: ts must be non-decreasing");
    }
    last_ts = ts[i];

    // Unresolvable leaves (e.g. a sample with no stack) are skipped; open
    // segments are kept as-is and the gap is not attributed to any frame.
    const uint32_t leaf_row = tree.FindRow(leaf_id[i]);
    if (leaf_row == core::Tree::kNullParent) {
      continue;
    }

    path.clear();
    for (uint32_t r = leaf_row; r != core::Tree::kNullParent;
         r = tree.parent[r]) {
      path.push_back(r);
    }

    const size_t old_depth = open.size();
    const size_t new_depth = path.size();

    // Length of the common prefix counting from the root (outermost frame):
    // segments at these depths continue, everything deeper diverges.
    size_t common = 0;
    const size_t max_cmp = std::min(old_depth, new_depth);
    while (common < max_cmp &&
           open[common].row == path[new_depth - 1 - common]) {
      ++common;
    }

    // Close the divergent tail (deepest first for a stable output order).
    for (size_t d = old_depth; d > common; --d) {
      emit(open[d - 1], d - 1, ts[i]);
    }
    open.resize(common);
    // Open new segments for the divergent tail of the new stack.
    for (size_t d = common; d < new_depth; ++d) {
      open.push_back(OpenSegment{path[new_depth - 1 - d], ts[i], /*count=*/0});
    }
    // Every depth of the point's stack gains one sample.
    for (size_t d = 0; d < new_depth; ++d) {
      ++open[d].count;
    }
  }

  // Close whatever is still open at the final point timestamp.
  for (size_t d = 0; d < open.size(); ++d) {
    emit(open[d], d, last_ts);
  }

  return std::move(builder).Build();
}

}  // namespace perfetto::trace_processor::flamechart
