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

#include "src/trace_processor/plugins/flamegraph/flamegraph.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::flamegraph {
namespace {

using core::FlexVector;

// Show-stack and show-from filters are tracked as per-frame bitmasks, one
// bit per filter, ORed along each path: a stack satisfies them when every
// bit is present.
constexpr size_t kMaxBitFilters = 63;

// Row helpers for the flattened metric columns; explicit loops so they
// inline instead of dispatching into libc for a handful of bytes.
void AddRow(double* dst, const double* src, uint32_t k) {
  for (uint32_t m = 0; m < k; ++m) {
    dst[m] += src[m];
  }
}

void CopyRow(double* dst, const double* src, uint32_t k) {
  for (uint32_t m = 0; m < k; ++m) {
    dst[m] = src[m];
  }
}

bool AnyNonZero(const double* row, uint32_t k) {
  for (uint32_t m = 0; m < k; ++m) {
    if (row[m] < 0 || row[m] > 0) {
      return true;
    }
  }
  return false;
}

// The compiled filter regexes. The pivot pattern is an extra show-stack
// filter (only pivot-containing stacks are shown), remembered by index so
// pivot frames can be identified from the match bitmask.
struct Filters {
  std::vector<base::Regex> show_stack;
  std::vector<base::Regex> hide_stack;
  std::vector<base::Regex> show_from;
  std::vector<base::Regex> hide_frame;
  uint32_t pivot_idx = 0;
  uint64_t show_stack_mask = 0;
  uint64_t show_from_mask = 0;
  // A bit outside the mask: assigning it to a frame's bits makes its
  // stacks unable to ever satisfy the mask, implementing hide-stack.
  uint64_t impossible_bits = 0;

  bool any() const {
    return !show_stack.empty() || !hide_stack.empty() || !show_from.empty() ||
           !hide_frame.empty();
  }
};

base::StatusOr<Filters> CompileFilters(const Config& config) {
  Filters f;
  for (const auto& spec : config.filters) {
    ASSIGN_OR_RETURN(base::Regex re, base::Regex::Create(spec.pattern));
    switch (spec.kind) {
      case Config::FilterKind::kShowStack:
        f.show_stack.push_back(std::move(re));
        break;
      case Config::FilterKind::kHideStack:
        f.hide_stack.push_back(std::move(re));
        break;
      case Config::FilterKind::kShowFromFrame:
        f.show_from.push_back(std::move(re));
        break;
      case Config::FilterKind::kHideFrame:
        f.hide_frame.push_back(std::move(re));
        break;
    }
  }
  if (config.view == Config::View::kPivot) {
    if (!config.pivot_pattern) {
      return base::ErrStatus(
          "flamegraph: pivot pattern is required for the pivot view");
    }
    ASSIGN_OR_RETURN(base::Regex re,
                     base::Regex::Create(*config.pivot_pattern));
    f.pivot_idx = static_cast<uint32_t>(f.show_stack.size());
    f.show_stack.push_back(std::move(re));
  } else if (config.pivot_pattern) {
    return base::ErrStatus(
        "flamegraph: pivot pattern is only valid for the pivot view");
  }
  if (f.show_stack.size() > kMaxBitFilters ||
      f.show_from.size() > kMaxBitFilters) {
    return base::ErrStatus("flamegraph: too many filters (max %zu per kind)",
                           kMaxBitFilters);
  }
  f.show_stack_mask = (uint64_t(1) << f.show_stack.size()) - 1;
  f.show_from_mask = (uint64_t(1) << f.show_from.size()) - 1;
  f.impossible_bits = uint64_t(1) << f.show_stack.size();
  return f;
}

// Returns the frames in parents-before-children order, excluding frames
// unreachable from a root (dangling parent indices, cycles). Input that is
// already ordered, the common case, is detected and returned as the
// identity order with no index built.
FlexVector<uint32_t> TopoOrder(const Forest& forest) {
  auto n = static_cast<uint32_t>(forest.size());
  bool ordered = true;
  for (uint32_t i = 0; i < n && ordered; ++i) {
    uint32_t p = forest.parent[i];
    ordered = p == kNoParent || p < i;
  }
  auto order = FlexVector<uint32_t>::CreateWithSize(n);
  if (ordered) {
    for (uint32_t i = 0; i < n; ++i) {
      order[i] = i;
    }
    return order;
  }
  // Reverse the parent index into child lists with a count + scatter pass,
  // then DFS from the roots.
  auto child_offset = FlexVector<uint32_t>::CreateFilled(n + 2, 0);
  for (uint32_t i = 0; i < n; ++i) {
    if (forest.parent[i] < n) {
      child_offset[forest.parent[i] + 2]++;
    }
  }
  for (uint32_t i = 2; i < n + 2; ++i) {
    child_offset[i] += child_offset[i - 1];
  }
  auto child_list = FlexVector<uint32_t>::CreateWithSize(child_offset[n + 1]);
  for (uint32_t i = 0; i < n; ++i) {
    if (forest.parent[i] < n) {
      child_list[child_offset[forest.parent[i] + 1]++] = i;
    }
  }
  order.clear();
  FlexVector<uint32_t> stack;
  for (uint32_t i = n; i-- > 0;) {
    if (forest.parent[i] == kNoParent) {
      stack.push_back(i);
    }
  }
  while (!stack.empty()) {
    uint32_t i = stack.back();
    stack.pop_back();
    order.push_back(i);
    for (uint32_t c = child_offset[i + 1]; c-- > child_offset[i];) {
      stack.push_back(child_list[c]);
    }
  }
  return order;
}

// A merged tree under construction. Node identity is (parent node, key),
// resolved through one exact hash map; nodes are created in
// parents-before-children order by construction. |expected_nodes| sizes
// the map and node columns so steady-state growth never reallocates; pass
// 0 when the output is expected to be small (pivot views).
struct Trie {
  Trie(uint32_t metric_count, uint32_t expected_nodes)
      : k(metric_count),
        // 1.5x keeps the expected node count under the map's load limit.
        map(NextPow2(expected_nodes + expected_nodes / 2)) {
    parent.reserve(expected_nodes);
    rep_frame.reserve(expected_nodes);
    self.reserve(uint64_t(expected_nodes) * k);
    cumulative.reserve(uint64_t(expected_nodes) * k);
  }

  static size_t NextPow2(uint32_t v) {
    size_t p = 16;
    while (p < v) {
      p *= 2;
    }
    return p;
  }

  uint32_t ChildOf(uint32_t node, uint32_t key, uint32_t frame) {
    auto next = static_cast<uint32_t>(parent.size());
    auto [id, inserted] = map.Insert((uint64_t(node) << 32) | key, next);
    if (inserted) {
      parent.push_back(node);
      rep_frame.push_back(frame);
      self.push_back_multiple(0, k);
      cumulative.push_back_multiple(0, k);
    }
    return *id;
  }

  size_t size() const { return parent.size(); }

  uint32_t k;
  base::FlatHashMapV2<uint64_t, uint32_t> map;
  FlexVector<uint32_t> parent;
  FlexVector<uint32_t> rep_frame;
  FlexVector<double> self;
  FlexVector<double> cumulative;
};

// Drops every node whose subtree is zero on all metrics (uncounted or
// valueless stacks) and packs the survivors into the output Tree, along
// with the constituent index built from the (node, frame) pairs by a
// count + scatter pass.
Tree PackTree(Trie trie, const FlexVector<uint64_t>& node_frame_pairs) {
  auto nodes = static_cast<uint32_t>(trie.size());
  uint32_t k = trie.k;
  if (nodes == 0) {
    Tree tree;
    tree.metric_count = k;
    tree.constituents_offset = FlexVector<uint32_t>::CreateFilled(1, 0);
    return tree;
  }
  // A node stays if its own subtree has value; children come after their
  // parent, so one reverse scan propagates "has a kept descendant".
  auto keep = FlexVector<uint8_t>::CreateFilled(nodes, 0);
  for (uint32_t i = nodes; i-- > 0;) {
    keep[i] = keep[i] || AnyNonZero(&trie.cumulative[i * k], k);
    if (keep[i] && trie.parent[i] != kNoParent) {
      keep[trie.parent[i]] = 1;
    }
  }
  auto remap = FlexVector<uint32_t>::CreateWithSize(nodes);
  uint32_t packed = 0;
  for (uint32_t i = 0; i < nodes; ++i) {
    remap[i] = keep[i] ? packed++ : kNoParent;
  }

  Tree tree;
  tree.metric_count = k;
  if (packed == nodes) {
    // Nothing dropped: adopt the trie's columns as-is (remap is the
    // identity).
    tree.parent = std::move(trie.parent);
    tree.rep_frame = std::move(trie.rep_frame);
    tree.self = std::move(trie.self);
    tree.cumulative = std::move(trie.cumulative);
  } else {
    tree.parent = FlexVector<uint32_t>::CreateWithSize(packed);
    tree.rep_frame = FlexVector<uint32_t>::CreateWithSize(packed);
    tree.self = FlexVector<double>::CreateWithSize(uint64_t(packed) * k);
    tree.cumulative =
        FlexVector<double>::CreateWithSize(uint64_t(packed) * k);
    for (uint32_t i = 0; i < nodes; ++i) {
      if (!keep[i]) {
        continue;
      }
      uint32_t out = remap[i];
      tree.parent[out] =
          trie.parent[i] == kNoParent ? kNoParent : remap[trie.parent[i]];
      tree.rep_frame[out] = trie.rep_frame[i];
      CopyRow(&tree.self[out * k], &trie.self[i * k], k);
      CopyRow(&tree.cumulative[out * k], &trie.cumulative[i * k], k);
    }
  }

  tree.constituents_offset =
      FlexVector<uint32_t>::CreateFilled(packed + 1, 0);
  for (uint64_t pair : node_frame_pairs) {
    uint32_t node = remap[pair >> 32];
    if (node != kNoParent) {
      tree.constituents_offset[node + 1]++;
    }
  }
  for (uint32_t i = 1; i <= packed; ++i) {
    tree.constituents_offset[i] += tree.constituents_offset[i - 1];
  }
  tree.constituents =
      FlexVector<uint32_t>::CreateWithSize(tree.constituents_offset[packed]);
  if (packed != 0) {
    auto cursor = FlexVector<uint32_t>::CreateWithSize(packed);
    memcpy(cursor.data(), tree.constituents_offset.data(),
           packed * sizeof(uint32_t));
    for (uint64_t pair : node_frame_pairs) {
      uint32_t node = remap[pair >> 32];
      if (node != kNoParent) {
        tree.constituents[cursor[node]++] = static_cast<uint32_t>(pair);
      }
    }
  }
  return tree;
}

// The state shared by the phases of one Build() call. See Build() at the
// bottom for the phase sequence.
class Builder {
 public:
  Builder(const Forest& forest,
          const Config& config,
          const StringPool& pool,
          Filters filters)
      : forest_(forest),
        config_(config),
        pool_(pool),
        filters_(std::move(filters)),
        n_(static_cast<uint32_t>(forest.size())),
        k_(forest.metric_count),
        // Full top-down / bottom-up output has up to one node per frame;
        // pivot output is typically far smaller, so let that grow on
        // demand.
        expected_nodes_(config.view == Config::View::kPivot ? 0 : n_) {}

  Flamegraph Run() {
    EvaluateNameMasks();
    order_ = TopoOrder(forest_);
    EvaluatePaths();
    SelectAnchors();
    Flamegraph result;
    result.down.metric_count = k_;
    result.up.metric_count = k_;
    if (config_.view != Config::View::kBottomUp) {
      result.down = BuildDown();
    }
    if (config_.view != Config::View::kTopDown) {
      result.up = BuildUp();
    }
    return result;
  }

 private:
  // Per distinct name, its regex results across all filters.
  struct NameMasks {
    uint64_t show_stack_bits;
    uint64_t show_from_bits;
    bool shown;
  };

  const NameMasks& MasksOf(uint32_t frame) const {
    return masks_[forest_.name[frame]];
  }

  bool IsPivotFrame(uint32_t frame) const {
    return ((MasksOf(frame).show_stack_bits >> filters_.pivot_idx) & 1) != 0;
  }

  // Runs every filter regex against every distinct name once.
  void EvaluateNameMasks() {
    auto names = static_cast<uint32_t>(forest_.name_table.size());
    masks_ = FlexVector<NameMasks>::CreateWithSize(names);
    for (uint32_t t = 0; t < names; ++t) {
      masks_[t] = {0, 0, true};
      if (!filters_.any()) {
        continue;
      }
      NullTermStringView name = pool_.Get(forest_.name_table[t]);
      std::string_view view(name.data(), name.size());
      auto matches = [&](const base::Regex& re) {
        if (re.PartialMatch(view)) {
          return true;
        }
        if (forest_.match_offset.empty()) {
          return false;
        }
        for (uint32_t s = forest_.match_offset[t];
             s < forest_.match_offset[t + 1]; ++s) {
          NullTermStringView extra = pool_.Get(forest_.match_strings[s]);
          if (re.PartialMatch(std::string_view(extra.data(),
                                               extra.size()))) {
            return true;
          }
        }
        return false;
      };
      uint64_t sb = 0;
      for (uint32_t j = 0; j < filters_.show_stack.size(); ++j) {
        sb |= uint64_t(matches(filters_.show_stack[j])) << j;
      }
      bool hidden = std::any_of(filters_.hide_stack.begin(),
                                filters_.hide_stack.end(), matches);
      masks_[t].show_stack_bits = hidden ? filters_.impossible_bits : sb;
      for (uint32_t j = 0; j < filters_.show_from.size(); ++j) {
        masks_[t].show_from_bits |= uint64_t(matches(filters_.show_from[j]))
                                    << j;
      }
      masks_[t].shown = !std::any_of(filters_.hide_frame.begin(),
                                     filters_.hide_frame.end(), matches);
    }
  }

  // One pass along every path, in topological order. Per frame: whether it
  // is kept (hide-frame, show-from), its nearest kept ancestor, whether
  // its stack satisfies every show/hide-stack filter (so its values
  // count), and its metrics with hidden frames' values folded into the
  // kept ancestor.
  void EvaluatePaths() {
    kept_ = FlexVector<uint8_t>::CreateFilled(n_, 0);
    counted_ = FlexVector<uint8_t>::CreateFilled(n_, 0);
    ancestor_ = FlexVector<uint32_t>::CreateFilled(n_, kNoParent);
    auto sb_path = FlexVector<uint64_t>::CreateWithSize(n_);
    auto sf_path = FlexVector<uint64_t>::CreateWithSize(n_);
    folded_ = FlexVector<double>::CreateWithSize(uint64_t(n_) * k_);
    memcpy(folded_.data(), forest_.metrics.data(),
           uint64_t(n_) * k_ * sizeof(double));
    for (uint32_t i : order_) {
      uint32_t p = forest_.parent[i];
      bool has_parent = p != kNoParent;
      const NameMasks& m = MasksOf(i);
      sf_path[i] =
          (has_parent ? sf_path[p] : 0) | (m.shown ? m.show_from_bits : 0);
      kept_[i] = m.shown && sf_path[i] == filters_.show_from_mask;
      ancestor_[i] = !has_parent ? kNoParent : (kept_[p] ? p : ancestor_[p]);
      sb_path[i] =
          (has_parent ? sb_path[p] : 0) | (kept_[i] ? m.show_stack_bits : 0);
      counted_[i] = kept_[i] && sb_path[i] == filters_.show_stack_mask;
      if (!kept_[i] && ancestor_[i] != kNoParent) {
        AddRow(&folded_[ancestor_[i] * k_], &forest_.metrics[i * k_], k_);
      }
    }
  }

  // Selects the frames the merged trees grow from and re-root at. This is
  // the only place the views differ; every phase after this treats
  // anchors uniformly.
  void SelectAnchors() {
    anchor_ = FlexVector<uint8_t>::CreateFilled(n_, 0);
    for (uint32_t i : order_) {
      switch (config_.view) {
        case Config::View::kTopDown:
          // The roots of the kept forest.
          anchor_[i] = kept_[i] && ancestor_[i] == kNoParent;
          break;
        case Config::View::kBottomUp:
          // Every frame whose own values count: each one is a stack
          // sample whose caller chain the up tree attributes.
          anchor_[i] = counted_[i];
          break;
        case Config::View::kPivot:
          // Frames matching the pivot pattern; nested matches re-root.
          anchor_[i] = kept_[i] && IsPivotFrame(i);
          break;
      }
    }
  }

  // The downward half: one forward scan with a trie cursor. Anchors start
  // merged roots; every other kept frame merges under its ancestor's
  // node.
  Tree BuildDown() {
    Trie trie(k_, expected_nodes_);
    auto trie_of = FlexVector<uint32_t>::CreateFilled(n_, kNoParent);
    for (uint32_t i : order_) {
      if (!kept_[i]) {
        continue;
      }
      uint32_t node;
      if (anchor_[i]) {
        node = trie.ChildOf(kNoParent, forest_.key[i], i);
      } else if (ancestor_[i] != kNoParent &&
                 trie_of[ancestor_[i]] != kNoParent) {
        node = trie.ChildOf(trie_of[ancestor_[i]], forest_.key[i], i);
      } else {
        continue;  // Not under any anchor.
      }
      trie_of[i] = node;
      // Self shows the merged frames' own values regardless of stack
      // filters; only cumulative is limited to counted stacks.
      AddRow(&trie.self[node * k_], &folded_[i * k_], k_);
      if (counted_[i]) {
        AddRow(&trie.cumulative[node * k_], &folded_[i * k_], k_);
      }
    }
    // Cumulative: children were created after their parent, so one
    // reverse scan sums subtrees onto the counted bases.
    for (uint32_t i = static_cast<uint32_t>(trie.size()); i-- > 0;) {
      if (trie.parent[i] != kNoParent) {
        AddRow(&trie.cumulative[trie.parent[i] * k_],
               &trie.cumulative[i * k_], k_);
      }
    }
    FlexVector<uint64_t> pairs;
    for (uint32_t i = 0; i < n_; ++i) {
      if (trie_of[i] != kNoParent) {
        pairs.push_back((uint64_t(trie_of[i]) << 32) | i);
      }
    }
    return PackTree(std::move(trie), pairs);
  }

  // The upward half: walk each anchor's kept ancestor chain, descending
  // the trie by key and attributing the anchor's weight to every node on
  // the chain.
  Tree BuildUp() {
    FlexVector<double> weights = AnchorWeights();
    // Unlike the downward half, the up tree is usually much smaller than
    // the input (distinct caller suffixes); growing on demand keeps the
    // map compact and cache-resident.
    Trie trie(k_, 0);
    FlexVector<uint64_t> pairs;
    for (uint32_t i : order_) {
      if (!anchor_[i] || !AnyNonZero(&weights[i * k_], k_)) {
        continue;
      }
      uint32_t node = kNoParent;
      for (uint32_t f = i; f != kNoParent; f = ancestor_[f]) {
        node = trie.ChildOf(node, forest_.key[f], f);
        AddRow(&trie.cumulative[node * k_], &weights[i * k_], k_);
        AddRow(&trie.self[node * k_], &folded_[f * k_], k_);
        pairs.push_back((uint64_t(node) << 32) | f);
      }
    }
    return PackTree(std::move(trie), pairs);
  }

  // The weight each anchor carries up its caller chain: its counted
  // subtree total, accumulated bottom-up along the kept tree and stopping
  // at other anchors (the same re-rooting the downward half applies).
  // With bottom-up anchors, where every counted frame is an anchor, this
  // reduces to each frame's own values.
  FlexVector<double> AnchorWeights() {
    auto weights = FlexVector<double>::CreateFilled(uint64_t(n_) * k_, 0);
    for (uint32_t idx = static_cast<uint32_t>(order_.size()); idx-- > 0;) {
      uint32_t i = order_[idx];
      if (!kept_[i]) {
        continue;
      }
      if (counted_[i]) {
        AddRow(&weights[i * k_], &folded_[i * k_], k_);
      }
      if (!anchor_[i] && ancestor_[i] != kNoParent) {
        AddRow(&weights[ancestor_[i] * k_], &weights[i * k_], k_);
      }
    }
    return weights;
  }

  const Forest& forest_;
  const Config& config_;
  const StringPool& pool_;
  Filters filters_;
  uint32_t n_;
  uint32_t k_;
  uint32_t expected_nodes_;

  FlexVector<NameMasks> masks_;
  FlexVector<uint32_t> order_;   // Topological, unreachable frames absent.
  FlexVector<uint8_t> kept_;     // Survives hide-frame / show-from.
  FlexVector<uint8_t> counted_;  // Kept and stack satisfies every filter.
  FlexVector<uint8_t> anchor_;   // Roots the merged trees. SelectAnchors().
  FlexVector<uint32_t> ancestor_;  // Nearest kept ancestor, or kNoParent.
  FlexVector<double> folded_;      // Metrics, hidden frames folded in.
};

}  // namespace

base::StatusOr<Flamegraph> Build(const Forest& forest,
                                 const Config& config,
                                 const StringPool& pool) {
  auto n = static_cast<uint32_t>(forest.size());
  uint32_t k = forest.metric_count;
  if (k == 0) {
    return base::ErrStatus("flamegraph: at least one metric is required");
  }
  if (forest.key.size() != n || forest.name.size() != n ||
      forest.metrics.size() != uint64_t(n) * k) {
    return base::ErrStatus("flamegraph: forest column sizes do not match");
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (forest.name[i] >= forest.name_table.size()) {
      return base::ErrStatus("flamegraph: frame name index out of range");
    }
  }
  if (!forest.match_offset.empty() &&
      forest.match_offset.size() != forest.name_table.size() + 1) {
    return base::ErrStatus(
        "flamegraph: match_offset must have one entry per name plus one");
  }
  ASSIGN_OR_RETURN(Filters filters, CompileFilters(config));
  if (n == 0) {
    Flamegraph empty;
    empty.down.metric_count = k;
    empty.down.constituents_offset = FlexVector<uint32_t>::CreateFilled(1, 0);
    empty.up.metric_count = k;
    empty.up.constituents_offset = FlexVector<uint32_t>::CreateFilled(1, 0);
    return empty;
  }
  return Builder(forest, config, pool, std::move(filters)).Run();
}

base::StatusOr<core::dataframe::Dataframe> ToDataframe(
    const Flamegraph& flamegraph,
    const Forest& forest,
    StringPool* pool) {
  uint32_t k = forest.metric_count;
  std::vector<std::string> columns = {"id", "parentId", "depth", "name"};
  for (uint32_t m = 0; m < k; ++m) {
    std::string suffix = k == 1 ? "" : std::to_string(m);
    columns.push_back("selfValue" + suffix);
    columns.push_back("cumulativeValue" + suffix);
  }
  // Sums of integral inputs are integral, so inspecting the output values
  // decides each metric's column type.
  std::vector<bool> integral(k, true);
  for (const Tree* tree : {&flamegraph.down, &flamegraph.up}) {
    for (const FlexVector<double>* col : {&tree->self, &tree->cumulative}) {
      for (uint64_t i = 0; i < col->size(); ++i) {
        double v = (*col)[i];
        double t = std::trunc(v);
        if (t < v || t > v) {
          integral[i % k] = false;
        }
      }
    }
  }

  core::dataframe::AdhocDataframeBuilder builder(columns, pool);
  bool ok = true;
  int64_t base = 0;
  for (const Tree* tree : {&flamegraph.down, &flamegraph.up}) {
    bool up = tree == &flamegraph.up;
    std::vector<int64_t> depth(tree->size());
    for (uint32_t i = 0; i < tree->size(); ++i) {
      bool root = tree->parent[i] == kNoParent;
      depth[i] = root ? 1 : depth[tree->parent[i]] + 1;
      ok = ok && builder.PushNonNull(0, base + i);
      ok = ok && builder.PushNonNull(
                     1, root ? int64_t(-1) : base + tree->parent[i]);
      ok = ok && builder.PushNonNull(2, up ? -depth[i] : depth[i]);
      ok = ok && builder.PushNonNull(
                     3, forest.name_table[forest.name[tree->rep_frame[i]]]);
      for (uint32_t m = 0; m < k; ++m) {
        double self = tree->self[i * k + m];
        double cumulative = tree->cumulative[i * k + m];
        if (integral[m]) {
          ok = ok && builder.PushNonNull(4 + 2 * m,
                                         static_cast<int64_t>(self));
          ok = ok && builder.PushNonNull(5 + 2 * m,
                                         static_cast<int64_t>(cumulative));
        } else {
          ok = ok && builder.PushNonNull(4 + 2 * m, self);
          ok = ok && builder.PushNonNull(5 + 2 * m, cumulative);
        }
      }
    }
    base += static_cast<int64_t>(tree->size());
  }
  if (!ok) {
    return builder.status();
  }
  ASSIGN_OR_RETURN(core::dataframe::Dataframe df,
                   std::move(builder).Build());
  df.Finalize();
  return df;
}

}  // namespace perfetto::trace_processor::flamegraph
