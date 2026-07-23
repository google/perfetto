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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_FLAMEGRAPH_FLAMEGRAPH_TREE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_FLAMEGRAPH_FLAMEGRAPH_TREE_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::flamegraph {

// Computes merged flamegraph trees from a forest of stack frames.
//
// A flamegraph is the trie of key-paths: every path through the input
// forest with the same sequence of keys collapses into one merged node.
// This library computes that trie and nothing else: no sibling ordering,
// no x layout, no output formatting. Those are cheap post-passes over the
// (much smaller) merged tree and belong to downstream consumers.
//
// All columns are structure-of-arrays; per-frame and per-node metric
// values are flattened row-major with a constant stride of |metric_count|
// values per row.

constexpr uint32_t kNoParent = std::numeric_limits<uint32_t>::max();

// A forest of stack frames, column-oriented: index i across all vectors
// describes frame i. Frames reference parents by index; kNoParent marks
// roots. Any frame order is accepted; parents-before-children order is
// detected and used as a fast path. Frames unreachable from a root
// (dangling parent indices, cycles) are ignored.
struct Forest {
  core::FlexVector<uint32_t> parent;
  // Merge identity: frames merge iff their path of keys from the root
  // matches. Callers fold whatever defines identity (name, mapping, ...)
  // into this one interned value.
  core::FlexVector<uint32_t> key;
  // The distinct display names occurring in the forest, and per frame an
  // index into that table. Filters and the pivot pattern match against
  // each distinct name once, so builders should dedup names here (they
  // intern them anyway).
  core::FlexVector<StringPool::Id> name_table;
  core::FlexVector<uint32_t> name;
  // Optional extra strings filters also match against, per name_table
  // entry: match_strings[match_offset[t]..match_offset[t + 1]). Builders
  // that fold extra dimensions into entries (e.g. property values merged
  // into the key) list them here so filters see them too. Empty means
  // names match alone; otherwise match_offset has name_table size + 1
  // entries.
  core::FlexVector<StringPool::Id> match_strings;
  core::FlexVector<uint32_t> match_offset;
  // Self values: metric m of frame i is metrics[i * metric_count + m].
  // All metrics are summed through the merge; a merged subtree is dropped
  // only when every metric sums to zero across it. Must be >= 1.
  uint32_t metric_count = 1;
  core::FlexVector<double> metrics;

  size_t size() const { return parent.size(); }
};

struct Config {
  enum class View : uint8_t { kTopDown, kBottomUp, kPivot };
  enum class FilterKind : uint8_t {
    // Keep only stacks where some frame matches.
    kShowStack,
    // Drop stacks where any frame matches.
    kHideStack,
    // Drop frames above the first match on each stack.
    kShowFromFrame,
    // Drop matching frames, folding their self values into the nearest
    // kept ancestor.
    kHideFrame,
  };
  struct Filter {
    FilterKind kind;
    // A regex (ECMAScript syntax) matched against frame names.
    std::string pattern;
  };

  View view = View::kTopDown;
  // Regex selecting the pivot frames. Must be set iff |view| is kPivot.
  std::optional<std::string> pivot_pattern;
  std::vector<Filter> filters;
};

// A merged tree. Nodes are stored parents-before-children in creation
// order; no sibling order is implied.
struct Tree {
  core::FlexVector<uint32_t> parent;  // kNoParent for roots.
  // A representative input frame per node (the first one merged in),
  // giving downstream access to names and any other per-frame data.
  core::FlexVector<uint32_t> rep_frame;
  // Metric sums, flattened like Forest::metrics: metric m of node n is
  // self[n * metric_count + m]. |self| sums the merged frames' own
  // values regardless of stack filters; |cumulative| sums the node's
  // subtree counting only frames whose stack satisfies every filter. Going up (bottom-up /
  // above-pivot), |cumulative| is instead the sum of the originating
  // anchors' weights: the classic bottom-up "attributed to this caller
  // chain" value.
  uint32_t metric_count = 1;
  core::FlexVector<double> self;
  core::FlexVector<double> cumulative;
  // The input frames merged into each node:
  // constituents[constituents_offset[n]..constituents_offset[n + 1]).
  // Going up a frame appears once per stack it contributes through, so
  // duplicates encode multiplicity.
  core::FlexVector<uint32_t> constituents_offset;  // size() + 1 entries.
  core::FlexVector<uint32_t> constituents;

  size_t size() const { return parent.size(); }
};

// The result: a downward tree (top-down view, or the subtrees hanging off
// pivot frames) and an upward tree (bottom-up view, or the caller chains
// of pivot frames). Views populate the halves they define; the other is
// empty.
struct Flamegraph {
  Tree down;
  Tree up;
};

base::StatusOr<Flamegraph> Build(const Forest& forest,
                                 const Config& config,
                                 const StringPool& pool);

// Converts the merged trees into one flat dataframe: one row per node,
// parents before children, with columns
//   id, parentId (-1 for roots), depth (1, 2, ... going down and
//   -1, -2, ... going up), name, and per metric selfValue and
//   cumulativeValue (suffixed with the metric index when there is more
//   than one).
// A metric's columns are LONG when all its values are integral, DOUBLE
// otherwise. Presentation (sibling order, x extents) is deliberately
// absent: consumers compute it over this far smaller output, or consume
// the trees directly.
base::StatusOr<core::dataframe::Dataframe> ToDataframe(
    const Flamegraph& flamegraph,
    const Forest& forest,
    StringPool* pool);

}  // namespace perfetto::trace_processor::flamegraph

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_FLAMEGRAPH_FLAMEGRAPH_TREE_H_
