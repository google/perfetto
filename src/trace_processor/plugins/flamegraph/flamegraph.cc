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
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/tree/tree_column_ops.h"
#include "src/trace_processor/core/tree/tree_path_interner.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::flamegraph {
namespace {

// A bit outside the SHOW_STACK mask. Once set, equality with the required
// SHOW_STACK mask is impossible, rejecting this path and all its descendants.
constexpr uint64_t kHideStackBit = uint64_t{1} << 63;

template <typename T>
bool TryAdd(T value, T* total) {
  static_assert(std::is_same_v<T, int64_t> || std::is_same_v<T, double>);
  if constexpr (std::is_same_v<T, int64_t>) {
    if ((value > 0 && *total > std::numeric_limits<int64_t>::max() - value) ||
        (value < 0 && *total < std::numeric_limits<int64_t>::min() - value)) {
      return false;
    }
  }
  *total += value;
  return true;
}

template <typename T>
bool TryAddNonNegative(T value, T* total) {
  static_assert(std::is_same_v<T, int64_t> || std::is_same_v<T, double>);
  PERFETTO_DCHECK(value >= 0);
  if constexpr (std::is_same_v<T, int64_t>) {
    if (value > std::numeric_limits<int64_t>::max() - *total) {
      return false;
    }
  }
  *total += value;
  return true;
}

template <typename T>
bool AccumulateSum(T value,
                   uint32_t destination,
                   core::Span<T> sums,
                   core::BitVector* has_value) {
  PERFETTO_DCHECK(destination < sums.size());
  PERFETTO_DCHECK(has_value->size() == sums.size());
  if (!has_value->is_set(destination)) {
    sums[destination] = value;
    has_value->set(destination);
    return true;
  }
  return TryAdd(value, &sums[destination]);
}

template <typename T>
bool PropagateNonNegativeSumToParents(const core::TreePathInterner& tree,
                                      core::Span<T> values) {
  PERFETTO_DCHECK(tree.size() == values.size());
  for (uint32_t node = tree.size(); node-- > 0;) {
    const uint32_t parent = tree.parent(node);
    if (parent != core::Tree::kNullParent &&
        !TryAddNonNegative(values[node], &values[parent])) {
      return false;
    }
  }
  return true;
}

template <typename T>
core::Slab<T> AllocFilled(uint64_t size, T value) {
  core::Slab<T> slab = core::Slab<T>::Alloc(size);
  std::fill_n(slab.data(), size, value);
  return slab;
}

void UpdateNonZeroRows(const core::Tree::Column& column,
                       core::Span<uint8_t> rows) {
  if (column.type.Is<core::Int64>()) {
    core::Span<const int64_t> values = column.unchecked_span<int64_t>();
    PERFETTO_DCHECK(values.size() == rows.size());
    for (uint32_t row = 0; row < rows.size(); ++row) {
      rows[row] |= values[row] != 0;
    }
    return;
  }
  PERFETTO_DCHECK(column.type.Is<core::Double>());
  core::Span<const double> values = column.unchecked_span<double>();
  PERFETTO_DCHECK(values.size() == rows.size());
  for (uint32_t row = 0; row < rows.size(); ++row) {
    rows[row] |= std::fpclassify(values[row]) != FP_ZERO;
  }
}

struct FilterMatches {
  uint64_t show_stack : Config::kMaxShowStackFilters;
  uint64_t view_pattern : 1;
  uint64_t hide_stack : 1;
  uint64_t hide_frame : 1;
};
static_assert(sizeof(FilterMatches) == 8);

constexpr FilterMatches kNoFilterMatches{};

void MergeFilterMatches(const FilterMatches& source,
                        FilterMatches* destination) {
  destination->show_stack |= source.show_stack;
  destination->view_pattern |= source.view_pattern;
  destination->hide_stack |= source.hide_stack;
  destination->hide_frame |= source.hide_frame;
}

// Everything later phases need from the root-to-row path traversal. Keeping
// the path-compressed retained frame avoids any later ancestor search.
struct PathState {
  // SHOW_STACK matches on retained frames, plus kHideStackBit if a retained
  // frame on this path matched HIDE_STACK. Removed frames inherit this value
  // unchanged, so their names cannot affect stack filtering.
  uint64_t stack_bits;

  // The nearest retained input row. A retained frame points to itself, while a
  // removed frame inherits its nearest retained ancestor.
  uint32_t retained_frame;

  // Node in the merged downward tree which receives this row's values.
  // kNullParent means this path is outside the downward half of the view.
  uint32_t downward_node;

  bool IsRetained(uint32_t row) const;
  bool StackIsHidden() const;
  bool StackContributes(uint64_t required_show_stack_bits) const;
  bool ContributesToDownwardCumulative(uint64_t required_show_stack_bits) const;
};
static_assert(sizeof(PathState) == 16);

static constexpr PathState kEmptyPathState =
    PathState{0, core::Tree::kNullParent, core::Tree::kNullParent};

struct UpwardPath {
  uint32_t root;
  uint32_t leaf;
};
static_assert(sizeof(UpwardPath) == 8);

struct UpwardAnchor {
  uint32_t frame;
  UpwardPath path;
};
static_assert(sizeof(UpwardAnchor) == 12);

struct TreeConstituent {
  uint32_t frame;
  uint32_t node;
};
static_assert(sizeof(TreeConstituent) == 8);

struct MetricColumns {
  core::Tree::Column self;
  core::Tree::Column cumulative;
};

struct AggregateColumns {
  core::Tree::Column downward;
  core::Tree::Column upward;
};

struct PackedTree {
  explicit PackedTree(uint32_t capacity)
      : depths(core::Slab<int64_t>::Alloc(capacity)),
        source_nodes(core::Slab<uint32_t>::Alloc(capacity)),
        parents(core::Slab<uint32_t>::Alloc(capacity)),
        representative_frames(core::Slab<uint32_t>::Alloc(capacity)) {}

  void Append(int64_t depth,
              uint32_t source_node,
              uint32_t parent,
              uint32_t representative_frame) {
    PERFETTO_DCHECK(rows < depths.size());
    depths[rows] = depth;
    source_nodes[rows] = source_node;
    parents[rows] = parent;
    representative_frames[rows] = representative_frame;
    ++rows;
  }

  uint32_t size() const { return rows; }
  core::Span<const int64_t> depth_span() const {
    return depths.span().subspan(0, rows);
  }
  core::Span<const uint32_t> source_node_span() const {
    return source_nodes.span().subspan(0, rows);
  }
  core::Span<const uint32_t> parent_span() const {
    return parents.span().subspan(0, rows);
  }
  core::Span<const uint32_t> representative_frame_span() const {
    return representative_frames.span().subspan(0, rows);
  }

  core::Slab<int64_t> depths;
  core::Slab<uint32_t> source_nodes;
  core::Slab<uint32_t> parents;
  core::Slab<uint32_t> representative_frames;
  uint32_t rows = 0;
};

// Owns all temporary state for one flamegraph computation. The methods are the
// algorithm phases in execution order; helpers are outlined below to keep the
// class declaration readable.
class FlamegraphBuilder {
 public:
  FlamegraphBuilder(core::Tree& input, const Config& config);

  base::StatusOr<core::Tree> Run();

 private:
  FilterMatches MatchText(const char* value) const;

  template <typename T>
  FilterMatches MatchNumber(T value) const;

  FilterMatches MatchColumn(const core::Tree::Column& column,
                            uint32_t row) const;
  FilterMatches MatchFrame(uint32_t row) const;
  void PrepareFrameHashes();
  void BuildDownwardStructure();
  base::Status BuildUpwardStructure();
  UpwardPath BuildUpwardPath(uint32_t frame, bool include_aggregates);
  base::Status AccumulateMetrics();
  base::Status ComputeAggregates();
  base::StatusOr<core::Tree> PackOutput();

  template <typename T>
  base::Status MarkActiveBottomUpAnchors(const core::Tree::Column& column,
                                         core::Span<uint8_t> active);

  template <typename T>
  static void InitializeMetricColumns(uint32_t rows, MetricColumns* output);

  template <typename T>
  base::Status AccumulateMetric(const core::Tree::Column& input,
                                MetricColumns* downward,
                                MetricColumns* upward);

  template <typename T>
  base::Status AccumulateDownwardMetric(const core::Tree::Column& input,
                                        MetricColumns* output);

  template <typename T>
  base::Status AccumulateUpwardMetric(const core::Tree::Column& input,
                                      MetricColumns* output);

  template <typename T>
  static base::Status PropagateCumulative(const core::TreePathInterner& tree,
                                          core::Span<T> cumulative);

  template <typename T>
  base::Status SumAggregateColumn(const core::Tree::Column& input,
                                  AggregateColumns* output);

  void AppendPackedNodes(const core::TreePathInterner& tree,
                         core::Span<const MetricColumns> metrics,
                         bool upward,
                         PackedTree* output) const;

  core::Tree& input_;
  const Config& config_;
  uint64_t required_show_stack_bits_;

  std::vector<base::MurmurHashCombiner> frame_hashes_;
  core::TreePathInterner downward_tree_;
  core::TreePathInterner upward_tree_;
  core::Slab<PathState> path_state_;
  core::FlexVector<UpwardAnchor> pivot_anchors_;
  core::FlexVector<TreeConstituent> upward_aggregate_constituents_;
  core::Slab<UpwardPath> bottom_up_paths_;
  std::vector<MetricColumns> downward_metrics_;
  std::vector<MetricColumns> upward_metrics_;
  std::vector<AggregateColumns> aggregate_columns_;
};

bool IsValidConfig(const Config& config) {
  if (!config.name || !config.name->type.Is<core::String>()) {
    return false;
  }
  if (config.value_columns.empty()) {
    return false;
  }
  for (const Config::AggregateColumn& aggregate : config.aggregate_columns) {
    if (!aggregate.input || aggregate.output_name.empty()) {
      return false;
    }
    if (aggregate.aggregate == Config::Aggregate::kSum &&
        !IsNumericColumn(*aggregate.input)) {
      return false;
    }
  }
  if (config.show_stack_filters.size() > Config::kMaxShowStackFilters) {
    return false;
  }
  const bool is_pattern_view = config.view == Config::View::kPivot ||
                               config.view == Config::View::kFromFrame;
  return is_pattern_view == config.view_pattern.has_value();
}

bool PathState::IsRetained(uint32_t row) const {
  return retained_frame == row;
}

bool PathState::StackIsHidden() const {
  return (stack_bits & kHideStackBit) != 0;
}

bool PathState::StackContributes(uint64_t required_show_stack_bits) const {
  return retained_frame != core::Tree::kNullParent &&
         stack_bits == required_show_stack_bits;
}

bool PathState::ContributesToDownwardCumulative(
    uint64_t required_show_stack_bits) const {
  return downward_node != core::Tree::kNullParent &&
         StackContributes(required_show_stack_bits);
}

FlamegraphBuilder::FlamegraphBuilder(core::Tree& input, const Config& config)
    : input_(input),
      config_(config),
      required_show_stack_bits_(
          (uint64_t{1} << config.show_stack_filters.size()) - 1),
      downward_tree_(config.view == Config::View::kTopDown ? input.row_count
                                                           : 0),
      upward_tree_(config.view == Config::View::kBottomUp ? input.row_count
                                                          : 0) {}

base::StatusOr<core::Tree> FlamegraphBuilder::Run() {
  PrepareFrameHashes();
  BuildDownwardStructure();
  RETURN_IF_ERROR(BuildUpwardStructure());
  std::vector<base::MurmurHashCombiner>().swap(frame_hashes_);
  RETURN_IF_ERROR(AccumulateMetrics());
  bottom_up_paths_ = {};
  pivot_anchors_ = {};
  RETURN_IF_ERROR(ComputeAggregates());
  path_state_ = {};
  upward_aggregate_constituents_ = {};
  return PackOutput();
}

FilterMatches FlamegraphBuilder::MatchText(const char* value) const {
  FilterMatches matches{};
  if (config_.view_pattern) {
    matches.view_pattern = config_.view_pattern->PartialMatch(value);
  }
  for (size_t i = 0; i < config_.show_stack_filters.size(); ++i) {
    if (config_.show_stack_filters[i].PartialMatch(value)) {
      matches.show_stack |= uint64_t{1} << i;
    }
  }
  for (const base::Regex& filter : config_.hide_stack_filters) {
    if (filter.PartialMatch(value)) {
      matches.hide_stack = 1;
      break;
    }
  }
  for (const base::Regex& filter : config_.hide_frame_filters) {
    if (filter.PartialMatch(value)) {
      matches.hide_frame = 1;
      break;
    }
  }
  return matches;
}

void FlamegraphBuilder::PrepareFrameHashes() {
  frame_hashes_.resize(input_.row_count);
  core::Span<base::MurmurHashCombiner> hashes =
      core::MakeMutableSpan(frame_hashes_);
  core::tree_ops::UpdateRowHashes(*config_.name, hashes);
  for (const core::Tree::Column* column : config_.grouping_columns) {
    core::tree_ops::UpdateRowHashes(*column, hashes);
  }
}

template <typename T>
FilterMatches FlamegraphBuilder::MatchNumber(T value) const {
  if constexpr (std::is_same_v<T, int64_t>) {
    return MatchText(base::StackString<32>("%" PRId64, value).c_str());
  } else {
    return MatchText(base::StackString<32>("%.15g", value).c_str());
  }
}

FilterMatches FlamegraphBuilder::MatchColumn(const core::Tree::Column& column,
                                             uint32_t row) const {
  if (column.null_bv.size() > 0 && !column.null_bv.is_set(row)) {
    return kNoFilterMatches;
  }
  if (column.type.Is<core::String>()) {
    const StringPool::Id value = column.unchecked_data<StringPool::Id>()[row];
    return value.is_null() ? kNoFilterMatches
                           : MatchText(config_.pool.Get(value).c_str());
  }
  if (column.type.Is<core::Int64>()) {
    return MatchNumber(column.unchecked_data<int64_t>()[row]);
  }
  return MatchNumber(column.unchecked_data<double>()[row]);
}

FilterMatches FlamegraphBuilder::MatchFrame(uint32_t row) const {
  FilterMatches matches = MatchColumn(*config_.name, row);
  for (const core::Tree::Column* column : config_.grouping_columns) {
    MergeFilterMatches(MatchColumn(*column, row), &matches);
  }
  return matches;
}

void FlamegraphBuilder::BuildDownwardStructure() {
  path_state_ = core::Slab<PathState>::Alloc(input_.row_count);
  const bool needs_matching = config_.HasFilters() || config_.view_pattern;
  base::FlatHashMapV2<uint64_t, uint32_t, base::AlreadyHashed<uint64_t>>
      frame_match_index;
  base::FlatHashMapV2<StringPool::Id, uint32_t> name_match_index;
  std::vector<FilterMatches> match_pool;
  const StringPool::Id* names = config_.name->unchecked_data<StringPool::Id>();

  for (uint32_t row = 0; row < input_.row_count; ++row) {
    // Tree guarantees parents precede children, so inheritance is an O(1)
    // reference rather than an ancestor walk or a copy. Roots inherit the
    // shared empty path.
    const uint32_t parent = input_.parent[row];
    const PathState& inheritance = parent == core::Tree::kNullParent
                                       ? kEmptyPathState
                                       : path_state_[parent];
    PERFETTO_DCHECK(parent == core::Tree::kNullParent || parent < row);

    // A HIDE_FRAME row folds into the same downward node as its parent and
    // cannot affect stack filtering.
    PathState& state = path_state_[row];
    const FilterMatches* matches = &kNoFilterMatches;
    if (needs_matching) {
      const uint32_t next_index = static_cast<uint32_t>(match_pool.size());
      uint32_t* index;
      bool inserted;
      StringPool::Id matched_name;
      if (config_.grouping_columns.empty()) {
        const bool is_null = config_.name->null_bv.size() > 0 &&
                             !config_.name->null_bv.is_set(row);
        matched_name = is_null ? StringPool::Id() : names[row];
        std::tie(index, inserted) =
            name_match_index.Insert(matched_name, next_index);
      } else {
        const uint64_t frame_hash = frame_hashes_[row].digest();
        std::tie(index, inserted) =
            frame_match_index.Insert(frame_hash, next_index);
      }
      if (inserted) {
        if (config_.grouping_columns.empty()) {
          match_pool.push_back(
              matched_name.is_null()
                  ? kNoFilterMatches
                  : MatchText(config_.pool.Get(matched_name).c_str()));
        } else {
          match_pool.push_back(MatchFrame(row));
        }
      }
      matches = &match_pool[*index];
    }
    if (matches->hide_frame) {
      state = inheritance;
      PERFETTO_DCHECK(!state.IsRetained(row));
      continue;
    }

    // Every other row is retained, even when it is outside the downward half.
    // This lets matching descendants start roots and lets upward paths walk
    // through retained ancestors.
    state.retained_frame = row;
    if (config_.view == Config::View::kPivot && matches->view_pattern) {
      pivot_anchors_.push_back(UpwardAnchor{
          row, {core::Tree::kNullParent, core::Tree::kNullParent}});
    }

    state.stack_bits = matches->hide_stack
                           ? kHideStackBit
                           : matches->show_stack | inheritance.stack_bits;

    // Top-down starts at every retained root. Pivot and from-frame start only
    // at matching frames; nested matches deliberately start another root.
    // Bottom-up has no downward half.
    const bool has_retained_parent =
        inheritance.retained_frame != core::Tree::kNullParent;
    const bool has_downward_parent =
        inheritance.downward_node != core::Tree::kNullParent;
    const bool is_top_down_root =
        config_.view == Config::View::kTopDown && !has_retained_parent;
    const bool starts_downward_tree = matches->view_pattern || is_top_down_root;
    if (!starts_downward_tree && !has_downward_parent) {
      state.downward_node = core::Tree::kNullParent;
      continue;
    }
    const uint32_t downward_parent = starts_downward_tree
                                         ? core::Tree::kNullParent
                                         : inheritance.downward_node;

    // Equivalent frames under the same merged parent share a node. The first
    // row which creates the node remains its representative.
    state.downward_node =
        downward_tree_.Intern(downward_parent, frame_hashes_[row], row);
  }
}

template <typename T>
base::Status FlamegraphBuilder::MarkActiveBottomUpAnchors(
    const core::Tree::Column& column,
    core::Span<uint8_t> active) {
  core::Span<const T> values = column.unchecked_span<T>();
  for (uint32_t row = 0; row < input_.row_count; ++row) {
    if (column.null_bv.size() > 0 && !column.null_bv.is_set(row)) {
      continue;
    }
    const T value = values[row];
    if (value < 0) {
      return base::ErrStatus("flamegraph: value columns must be non-negative");
    }
    bool non_zero;
    if constexpr (std::is_same_v<T, int64_t>) {
      non_zero = value != 0;
    } else {
      non_zero = std::fpclassify(value) != FP_ZERO;
    }
    const PathState& state = path_state_[row];
    if (non_zero && state.StackContributes(required_show_stack_bits_)) {
      active[state.retained_frame] = 1;
    }
  }
  return base::OkStatus();
}

base::Status FlamegraphBuilder::BuildUpwardStructure() {
  if (config_.view == Config::View::kPivot) {
    for (UpwardAnchor& anchor : pivot_anchors_) {
      const bool include_aggregates =
          !config_.aggregate_columns.empty() &&
          path_state_[anchor.frame].StackContributes(required_show_stack_bits_);
      anchor.path = BuildUpwardPath(anchor.frame, include_aggregates);
    }
    return base::OkStatus();
  }
  if (config_.view != Config::View::kBottomUp) {
    return base::OkStatus();
  }

  auto active = AllocFilled<uint8_t>(input_.row_count, 0);
  for (const core::Tree::Column* column : config_.value_columns) {
    if (column->type.Is<core::Int64>()) {
      RETURN_IF_ERROR(
          MarkActiveBottomUpAnchors<int64_t>(*column, active.mutable_span()));
    } else {
      RETURN_IF_ERROR(
          MarkActiveBottomUpAnchors<double>(*column, active.mutable_span()));
    }
  }
  constexpr UpwardPath kNoPath{core::Tree::kNullParent,
                               core::Tree::kNullParent};
  bottom_up_paths_ = AllocFilled<UpwardPath>(input_.row_count, kNoPath);
  for (uint32_t row = 0; row < input_.row_count; ++row) {
    if (active[row]) {
      bottom_up_paths_[row] = BuildUpwardPath(row, true);
    }
  }
  return base::OkStatus();
}

UpwardPath FlamegraphBuilder::BuildUpwardPath(uint32_t frame,
                                              bool include_aggregates) {
  UpwardPath result{core::Tree::kNullParent, core::Tree::kNullParent};
  uint32_t upward_parent = core::Tree::kNullParent;
  while (frame != core::Tree::kNullParent) {
    upward_parent =
        upward_tree_.Intern(upward_parent, frame_hashes_[frame], frame);
    if (result.root == core::Tree::kNullParent) {
      result.root = upward_parent;
    }
    if (include_aggregates) {
      upward_aggregate_constituents_.push_back(
          TreeConstituent{frame, upward_parent});
    }
    const uint32_t parent = input_.parent[frame];
    frame = parent == core::Tree::kNullParent
                ? core::Tree::kNullParent
                : path_state_[parent].retained_frame;
  }
  result.leaf = upward_parent;
  return result;
}

template <typename T>
void FlamegraphBuilder::InitializeMetricColumns(uint32_t rows,
                                                MetricColumns* output) {
  output->self = core::Tree::Column::Create<T>(rows);
  output->cumulative = core::Tree::Column::Create<T>(rows);
  core::Span<T> self = output->self.unchecked_span<T>();
  core::Span<T> cumulative = output->cumulative.unchecked_span<T>();
  std::fill(self.begin(), self.end(), T{});
  std::fill(cumulative.begin(), cumulative.end(), T{});
}

template <typename T>
base::Status FlamegraphBuilder::AccumulateMetric(
    const core::Tree::Column& input,
    MetricColumns* downward,
    MetricColumns* upward) {
  RETURN_IF_ERROR(AccumulateDownwardMetric<T>(input, downward));
  return AccumulateUpwardMetric<T>(input, upward);
}

template <typename T>
base::Status FlamegraphBuilder::PropagateCumulative(
    const core::TreePathInterner& tree,
    core::Span<T> cumulative) {
  if (!PropagateNonNegativeSumToParents(tree, cumulative)) {
    return base::ErrStatus("flamegraph: integer value overflow");
  }
  return base::OkStatus();
}

template <typename T>
base::Status FlamegraphBuilder::AccumulateDownwardMetric(
    const core::Tree::Column& input,
    MetricColumns* output) {
  InitializeMetricColumns<T>(downward_tree_.size(), output);
  core::Span<T> self = output->self.unchecked_span<T>();
  core::Span<T> cumulative = output->cumulative.unchecked_span<T>();
  if (config_.view == Config::View::kBottomUp) {
    return base::OkStatus();
  }

  core::Span<const T> values = input.unchecked_span<T>();
  for (uint32_t row = 0; row < input_.row_count; ++row) {
    if (input.null_bv.size() > 0 && !input.null_bv.is_set(row)) {
      continue;
    }
    const T value = values[row];
    if (value < 0) {
      return base::ErrStatus("flamegraph: value columns must be non-negative");
    }

    const PathState& state = path_state_[row];
    if (state.downward_node == core::Tree::kNullParent) {
      continue;
    }
    if (state.ContributesToDownwardCumulative(required_show_stack_bits_) &&
        (!TryAddNonNegative(value, &self[state.downward_node]) ||
         !TryAddNonNegative(value, &cumulative[state.downward_node]))) {
      return base::ErrStatus("flamegraph: integer value overflow");
    }
  }

  return PropagateCumulative(downward_tree_, cumulative);
}

template <typename T>
base::Status FlamegraphBuilder::AccumulateUpwardMetric(
    const core::Tree::Column& input,
    MetricColumns* output) {
  InitializeMetricColumns<T>(upward_tree_.size(), output);
  core::Span<T> self = output->self.unchecked_span<T>();
  core::Span<T> cumulative = output->cumulative.unchecked_span<T>();

  if (config_.view != Config::View::kPivot &&
      config_.view != Config::View::kBottomUp) {
    return base::OkStatus();
  }

  core::Span<const T> values = input.unchecked_span<T>();
  if (config_.view == Config::View::kBottomUp) {
    for (uint32_t row = 0; row < input_.row_count; ++row) {
      if (input.null_bv.size() > 0 && !input.null_bv.is_set(row)) {
        continue;
      }
      const T value = values[row];
      bool is_zero;
      if constexpr (std::is_same_v<T, int64_t>) {
        is_zero = value == 0;
      } else {
        is_zero = std::fpclassify(value) == FP_ZERO;
      }
      const PathState& state = path_state_[row];
      if (is_zero || !state.StackContributes(required_show_stack_bits_)) {
        continue;
      }
      const uint32_t retained = state.retained_frame;
      const UpwardPath& path = bottom_up_paths_[retained];
      const uint32_t root = path.root;
      const uint32_t leaf = path.leaf;
      PERFETTO_DCHECK(root != core::Tree::kNullParent);
      PERFETTO_DCHECK(leaf != core::Tree::kNullParent);
      if (!TryAddNonNegative(value, &self[root]) ||
          !TryAddNonNegative(value, &cumulative[leaf])) {
        return base::ErrStatus("flamegraph: integer value overflow");
      }
    }
    return PropagateCumulative(upward_tree_, cumulative);
  }

  auto anchor_weights = AllocFilled<T>(input_.row_count, T{});
  for (uint32_t row = 0; row < input_.row_count; ++row) {
    if (input.null_bv.size() > 0 && !input.null_bv.is_set(row)) {
      continue;
    }
    const PathState& state = path_state_[row];
    if (state.ContributesToDownwardCumulative(required_show_stack_bits_) &&
        !TryAddNonNegative(values[row],
                           &anchor_weights[state.retained_frame])) {
      return base::ErrStatus("flamegraph: integer value overflow");
    }
  }

  for (uint32_t row = input_.row_count; row-- > 0;) {
    const PathState& state = path_state_[row];
    if (!state.IsRetained(row) ||
        state.downward_node == core::Tree::kNullParent ||
        downward_tree_.parent(state.downward_node) == core::Tree::kNullParent) {
      continue;
    }
    const uint32_t parent = input_.parent[row];
    PERFETTO_DCHECK(parent != core::Tree::kNullParent);
    const uint32_t retained_parent = path_state_[parent].retained_frame;
    PERFETTO_DCHECK(retained_parent != core::Tree::kNullParent);
    if (!TryAddNonNegative(anchor_weights[row],
                           &anchor_weights[retained_parent])) {
      return base::ErrStatus("flamegraph: integer value overflow");
    }
  }

  for (const UpwardAnchor& anchor : pivot_anchors_) {
    const T value = anchor_weights[anchor.frame];
    if (!TryAddNonNegative(value, &self[anchor.path.root]) ||
        !TryAddNonNegative(value, &cumulative[anchor.path.leaf])) {
      return base::ErrStatus("flamegraph: integer value overflow");
    }
  }
  return PropagateCumulative(upward_tree_, cumulative);
}

base::Status FlamegraphBuilder::AccumulateMetrics() {
  downward_metrics_.resize(config_.value_columns.size());
  upward_metrics_.resize(config_.value_columns.size());
  for (uint32_t i = 0; i < config_.value_columns.size(); ++i) {
    const core::Tree::Column& column = *config_.value_columns[i];
    switch (column.type.index()) {
      case core::Tree::Column::Type::GetTypeIndex<core::Int64>():
        RETURN_IF_ERROR(AccumulateMetric<int64_t>(column, &downward_metrics_[i],
                                                  &upward_metrics_[i]));
        break;
      case core::Tree::Column::Type::GetTypeIndex<core::Double>():
        RETURN_IF_ERROR(AccumulateMetric<double>(column, &downward_metrics_[i],
                                                 &upward_metrics_[i]));
        break;
      default:
        PERFETTO_FATAL("Unsupported flamegraph value column type");
    }
  }
  return base::OkStatus();
}

template <typename T>
base::Status FlamegraphBuilder::SumAggregateColumn(
    const core::Tree::Column& input,
    AggregateColumns* output) {
  auto retained_totals = AllocFilled<T>(input_.row_count, T{});
  auto retained_has_value =
      core::BitVector::CreateWithSize(input_.row_count, false);
  core::Span<const T> values = input.unchecked_span<T>();
  core::Span<T> retained_sums = retained_totals.mutable_span();
  for (uint32_t row = 0; row < input_.row_count; ++row) {
    if (input.null_bv.size() > 0 && !input.null_bv.is_set(row)) {
      continue;
    }
    const PathState& state = path_state_[row];
    const uint32_t retained = state.retained_frame;
    if (retained == core::Tree::kNullParent || state.StackIsHidden()) {
      continue;
    }
    if (!AccumulateSum(values[row], retained, retained_sums,
                       &retained_has_value)) {
      return base::ErrStatus("flamegraph: integer aggregate overflow");
    }
  }

  output->downward = core::Tree::Column::Create<T>(downward_tree_.size());
  output->downward.null_bv =
      core::BitVector::CreateWithSize(downward_tree_.size(), false);
  core::Span<T> downward = output->downward.unchecked_span<T>();
  for (uint32_t frame = 0; frame < input_.row_count; ++frame) {
    const PathState& state = path_state_[frame];
    if (!state.IsRetained(frame) || !retained_has_value.is_set(frame) ||
        !state.ContributesToDownwardCumulative(required_show_stack_bits_)) {
      continue;
    }
    const uint32_t node = state.downward_node;
    if (!AccumulateSum(retained_totals[frame], node, downward,
                       &output->downward.null_bv)) {
      return base::ErrStatus("flamegraph: integer aggregate overflow");
    }
  }

  output->upward = core::Tree::Column::Create<T>(upward_tree_.size());
  output->upward.null_bv =
      core::BitVector::CreateWithSize(upward_tree_.size(), false);
  core::Span<T> upward = output->upward.unchecked_span<T>();
  for (const TreeConstituent& constituent : upward_aggregate_constituents_) {
    const uint32_t frame = constituent.frame;
    const uint32_t node = constituent.node;
    if (!retained_has_value.is_set(frame)) {
      continue;
    }
    if (!AccumulateSum(retained_totals[frame], node, upward,
                       &output->upward.null_bv)) {
      return base::ErrStatus("flamegraph: integer aggregate overflow");
    }
  }
  return base::OkStatus();
}

base::Status FlamegraphBuilder::ComputeAggregates() {
  aggregate_columns_.resize(config_.aggregate_columns.size());
  for (uint32_t i = 0; i < config_.aggregate_columns.size(); ++i) {
    const core::Tree::Column& input = *config_.aggregate_columns[i].input;
    if (input.type.Is<core::Int64>()) {
      RETURN_IF_ERROR(
          SumAggregateColumn<int64_t>(input, &aggregate_columns_[i]));
    } else {
      RETURN_IF_ERROR(
          SumAggregateColumn<double>(input, &aggregate_columns_[i]));
    }
  }
  return base::OkStatus();
}

void FlamegraphBuilder::AppendPackedNodes(
    const core::TreePathInterner& tree,
    core::Span<const MetricColumns> metrics,
    bool upward,
    PackedTree* output) const {
  auto keep = AllocFilled<uint8_t>(tree.size(), 0);
  for (const MetricColumns& metric : metrics) {
    UpdateNonZeroRows(metric.cumulative, keep.mutable_span());
  }

  auto output_row = AllocFilled<uint32_t>(tree.size(), core::Tree::kNullParent);
  for (uint32_t node = 0; node < tree.size(); ++node) {
    if (!keep[node]) {
      continue;
    }
    const uint32_t parent = tree.parent(node);
    const uint32_t packed_parent = parent == core::Tree::kNullParent
                                       ? core::Tree::kNullParent
                                       : output_row[parent];
    PERFETTO_DCHECK(parent == core::Tree::kNullParent ||
                    packed_parent != core::Tree::kNullParent);
    const int64_t depth =
        packed_parent == core::Tree::kNullParent
            ? (upward ? -1 : 1)
            : output->depths[packed_parent] + (upward ? -1 : 1);
    output_row[node] = output->size();
    output->Append(depth, node, packed_parent, tree.representative_row(node));
  }
}

base::StatusOr<core::Tree> FlamegraphBuilder::PackOutput() {
  PackedTree packed(downward_tree_.size() + upward_tree_.size());
  AppendPackedNodes(downward_tree_, core::MakeSpan(downward_metrics_), false,
                    &packed);
  const uint32_t downward_rows = packed.size();
  AppendPackedNodes(upward_tree_, core::MakeSpan(upward_metrics_), true,
                    &packed);

  core::Tree output;
  output.row_count = packed.size();
  const size_t column_count = 2 + config_.grouping_columns.size() +
                              2 * config_.value_columns.size() +
                              config_.aggregate_columns.size();
  output.names.reserve(column_count);
  output.columns.reserve(column_count);
  output.parent = core::Slab<uint32_t>::Alloc(output.row_count);
  core::Span<const uint32_t> packed_parents = packed.parent_span();
  std::copy(packed_parents.begin(), packed_parents.end(),
            output.parent.begin());

  output.names.push_back("depth");
  core::Tree::Column depth =
      core::Tree::Column::Create<int64_t>(output.row_count);
  core::Span<int64_t> depths = depth.unchecked_span<int64_t>();
  core::Span<const int64_t> packed_depths = packed.depth_span();
  std::copy(packed_depths.begin(), packed_depths.end(), depths.begin());
  output.columns.push_back(std::move(depth));

  const core::Span<const uint32_t> representatives =
      packed.representative_frame_span();
  output.names.push_back("name");
  output.columns.push_back(
      core::tree_ops::Gather(*config_.name, representatives));
  for (const core::Tree::Column* grouping : config_.grouping_columns) {
    output.names.emplace_back(input_.ColumnName(grouping));
    output.columns.push_back(
        core::tree_ops::Gather(*grouping, representatives));
  }

  const core::Span<const uint32_t> packed_sources = packed.source_node_span();
  const core::Span<const uint32_t> downward_sources =
      packed_sources.subspan(0, downward_rows);
  const core::Span<const uint32_t> upward_sources = packed_sources.subspan(
      downward_rows, packed_sources.size() - downward_rows);
  auto gather_merged = [&](const core::Tree::Column& downward,
                           const core::Tree::Column& upward) {
    return core::tree_ops::GatherConcat(downward, downward_sources, upward,
                                        upward_sources);
  };
  for (uint32_t i = 0; i < config_.value_columns.size(); ++i) {
    const std::string name(input_.ColumnName(config_.value_columns[i]));
    output.names.push_back("self_" + name);
    output.columns.push_back(
        gather_merged(downward_metrics_[i].self, upward_metrics_[i].self));
    output.names.push_back("cumulative_" + name);
    output.columns.push_back(gather_merged(downward_metrics_[i].cumulative,
                                           upward_metrics_[i].cumulative));
  }
  for (uint32_t i = 0; i < config_.aggregate_columns.size(); ++i) {
    output.names.push_back(config_.aggregate_columns[i].output_name);
    output.columns.push_back(gather_merged(aggregate_columns_[i].downward,
                                           aggregate_columns_[i].upward));
  }
  return std::move(output);
}

}  // namespace

base::StatusOr<core::Tree> Build(core::Tree&& tree, const Config& config) {
  PERFETTO_DCHECK(IsValidConfig(config));
  return FlamegraphBuilder(tree, config).Run();
}

}  // namespace perfetto::trace_processor::flamegraph
