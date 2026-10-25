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

#include "src/trace_processor/core/exec/graph_tree.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/util/graph.h"

namespace perfetto::trace_processor::core::exec {

namespace {

constexpr uint32_t kNoParent = util::GraphTree::kNoParent;

const char* Name(GraphTreeSpec::Kind kind) {
  switch (kind) {
    case GraphTreeSpec::Kind::kBfs:
      return "GRAPH BFS TREE";
    case GraphTreeSpec::Kind::kDfs:
      return "GRAPH DFS TREE";
    case GraphTreeSpec::Kind::kDominator:
      return "GRAPH DOMINATOR TREE";
    case GraphTreeSpec::Kind::kCriticalPath:
      return "GRAPH CRITICAL PATH";
  }
  PERFETTO_FATAL("For GCC");
}

// Finds a node by its id. Ids which are close to dense, as table ids are, are
// looked up in an array, and any others in a hash map.
class NodeIndex {
 public:
  // Numbers each of `ids` by its place. Returns an id which appears twice, if
  // any.
  std::optional<int64_t> Build(const std::vector<int64_t>& ids) {
    if (ids.empty()) {
      return std::nullopt;
    }
    auto [min, max] = std::minmax_element(ids.begin(), ids.end());
    min_ = *min;
    uint64_t span = static_cast<uint64_t>(*max) - static_cast<uint64_t>(*min);
    dense_ = span < 2 * uint64_t{ids.size()} + 1024;
    if (dense_) {
      node_of_offset_.assign(span + 1, kNoNode);
    }
    for (uint32_t node = 0; node < ids.size(); ++node) {
      if (dense_) {
        uint32_t& slot = node_of_offset_[static_cast<uint64_t>(ids[node]) -
                                         static_cast<uint64_t>(min_)];
        if (slot != kNoNode) {
          return ids[node];
        }
        slot = node;
      } else if (!node_of_id_.Insert(ids[node], node).second) {
        return ids[node];
      }
    }
    return std::nullopt;
  }

  // The node with `id`, or kNoNode.
  PERFETTO_ALWAYS_INLINE uint32_t Find(int64_t id) const {
    if (dense_) {
      auto offset = static_cast<uint64_t>(id) - static_cast<uint64_t>(min_);
      return offset < node_of_offset_.size() ? node_of_offset_[offset]
                                             : kNoNode;
    }
    const uint32_t* node = node_of_id_.Find(id);
    return node ? *node : kNoNode;
  }

 private:
  bool dense_ = false;
  int64_t min_ = 0;
  std::vector<uint32_t> node_of_offset_;
  base::FlatHashMap<int64_t, uint32_t> node_of_id_;
};

}  // namespace

struct GraphTree::State : OperatorState {
  ~State() override;

  struct Seed {
    uint32_t node;
    int64_t ts;
    int64_t end;
    uint32_t seed_row;
  };
  struct LaneSpan {
    int64_t ts;
    int64_t end;
    uint32_t node;
    uint32_t dst_node;
  };
  struct StackFrame {
    uint32_t lane;
    uint32_t target_node;
    int64_t w_start;
    int64_t w_end;
    uint32_t depth;
    int64_t parent_id;
  };
  struct EmittedRow {
    int64_t ts;
    int64_t dur;
    int64_t parent_id;
    uint32_t node;
    uint32_t depth;
  };

  void Clear() {
    rows.Clear();
    seed_rows.Clear();
    computed = false;
    id_of_node.clear();
    tree = {};
    position.clear();
    node_ts.clear();
    node_end.clear();
    lane_of_node.clear();
    lane_start.clear();
    spans.clear();
    prefix_max_end.clear();
    lane_disjoint.clear();
    node_last_span.clear();
    seeds.clear();
    seed_idx = 0;
    group_num = 0;
    root_rows.clear();
    root_row_idx = 0;
    cur_root_id = 0;
    cur_seed_row = 0;
    cur_group = 0;
    served = 0;
    status = base::OkStatus();
  }

  std::unique_ptr<OperatorState> nodes_state;
  std::unique_ptr<OperatorState> edges_state;
  std::unique_ptr<OperatorState> seeds_state;
  Context* context = nullptr;

  // The retained columns of every node row, in the order they arrived.
  RowStore rows;
  RowStore seed_rows;
  bool computed = false;
  // Nodes are numbered in the order they arrived, so a node's number is also
  // its row in `rows`.
  std::vector<int64_t> id_of_node;
  util::GraphTree tree;
  // By node, its row's place in the output.
  std::vector<uint32_t> position;

  // CSR interval graph and streaming state for Kind::kCriticalPath.
  std::vector<int64_t> node_ts;
  std::vector<int64_t> node_end;
  std::vector<uint32_t> lane_of_node;
  std::vector<uint32_t> lane_start;
  std::vector<LaneSpan> spans;
  std::vector<int64_t> prefix_max_end;
  std::vector<bool> lane_disjoint;
  std::vector<uint32_t> node_last_span;
  std::vector<Seed> seeds;
  size_t seed_idx = 0;
  uint32_t group_num = 0;
  std::vector<StackFrame> stack;
  std::vector<EmittedRow> root_rows;
  size_t root_row_idx = 0;
  int64_t cur_root_id = 0;
  uint32_t cur_seed_row = 0;
  uint32_t cur_group = 0;

  uint32_t served = 0;
  base::Status status = base::OkStatus();

  // Scratch, reused across calls.
  RowBatch batch;
  RowBatch retained;
  RowBatch gathered;
  std::vector<uint32_t> gather_rows;
  std::vector<uint32_t> seed_gather_rows;
};

GraphTree::State::~State() = default;

GraphTree::GraphTree(GraphTreeSpec spec) : spec_(std::move(spec)) {}

GraphTree::~GraphTree() = default;

std::unique_ptr<OperatorState> GraphTree::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->context = &context;
  state->nodes_state = spec_.nodes->MakeState(context);
  state->edges_state = spec_.edges->MakeState(context);
  state->seeds_state = spec_.seeds->MakeState(context);
  return state;
}

void GraphTree::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  spec_.nodes->Rewind(*s.nodes_state);
  spec_.edges->Rewind(*s.edges_state);
  spec_.seeds->Rewind(*s.seeds_state);
  s.Clear();
}

base::Status GraphTree::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

base::Status GraphTree::BuildCriticalPath(State& s) const {
  const char* name = Name(spec_.kind);
  bool has_lane_col = spec_.node_lane_column != spec_.node_column;
  bool keep_rows = !spec_.retained_columns.empty();

  std::vector<int64_t>& node_ts = s.node_ts;
  std::vector<int64_t>& node_end = s.node_end;
  std::vector<int64_t> node_raw_lane;
  while (spec_.nodes->GetData(s.batch, *s.nodes_state)) {
    FlatColumnReader<int64_t> ids(s.batch, spec_.node_column);
    FlatColumnReader<int64_t> ts_col(s.batch, spec_.node_ts_column);
    FlatColumnReader<int64_t> dur_col(s.batch, spec_.node_dur_column);
    FlatColumnReader<int64_t> lane_col(s.batch, spec_.node_lane_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t id;
      if (!ids.Read(row, &id)) {
        return base::ErrStatus("%s: a node has no id", name);
      }
      int64_t ts = 0;
      int64_t dur = 0;
      if (!ts_col.Read(row, &ts) || !dur_col.Read(row, &dur)) {
        ts = 0;
        dur = 0;
      }
      int64_t end = dur > 0 ? ts + dur : ts;
      s.id_of_node.push_back(id);
      node_ts.push_back(ts);
      node_end.push_back(end);
      if (has_lane_col) {
        int64_t lane = id;
        lane_col.Read(row, &lane);
        node_raw_lane.push_back(lane);
      }
    }
    if (keep_rows) {
      s.retained.Project(s.batch, spec_.retained_columns);
      RETURN_IF_ERROR(s.rows.Append(s.retained));
    }
  }
  RETURN_IF_ERROR(spec_.nodes->status(*s.nodes_state));

  auto node_count = static_cast<uint32_t>(s.id_of_node.size());
  bool is_identity_id = true;
  int64_t min_id = 0;
  int64_t max_id = -1;
  if (node_count > 0) {
    min_id = s.id_of_node[0];
    max_id = s.id_of_node[0];
    for (uint32_t u = 0; u < node_count; ++u) {
      int64_t id = s.id_of_node[u];
      if (id != static_cast<int64_t>(u)) {
        is_identity_id = false;
      }
      min_id = std::min(min_id, id);
      max_id = std::max(max_id, id);
    }
  }
  uint64_t id_span = node_count > 0 ? static_cast<uint64_t>(max_id) -
                                          static_cast<uint64_t>(min_id)
                                    : 0;
  bool use_dense_id = !is_identity_id && node_count > 0 &&
                      id_span < 8 * uint64_t{node_count} + 4096;
  std::vector<uint32_t> dense_id;
  NodeIndex index;
  if (use_dense_id) {
    dense_id.assign(id_span + 1, kNoNode);
    for (uint32_t u = 0; u < node_count; ++u) {
      uint32_t& slot = dense_id[static_cast<uint64_t>(s.id_of_node[u]) -
                                static_cast<uint64_t>(min_id)];
      if (slot != kNoNode) {
        return base::ErrStatus("%s: two nodes have the id %" PRId64, name,
                               s.id_of_node[u]);
      }
      slot = u;
    }
  } else if (!is_identity_id) {
    if (std::optional<int64_t> twice = index.Build(s.id_of_node)) {
      return base::ErrStatus("%s: two nodes have the id %" PRId64, name,
                             *twice);
    }
  }
  auto find_node = [&](int64_t id) -> uint32_t {
    if (is_identity_id) {
      return (id >= 0 && static_cast<uint64_t>(id) < node_count)
                 ? static_cast<uint32_t>(id)
                 : kNoNode;
    }
    if (use_dense_id) {
      if (id < min_id || id > max_id) {
        return kNoNode;
      }
      return dense_id[static_cast<uint64_t>(id) -
                      static_cast<uint64_t>(min_id)];
    }
    return index.Find(id);
  };

  std::vector<uint32_t>& lane_of_node = s.lane_of_node;
  lane_of_node.resize(node_count);
  uint32_t num_lanes = 0;
  if (!has_lane_col) {
    for (uint32_t u = 0; u < node_count; ++u) {
      lane_of_node[u] = u;
    }
    num_lanes = node_count;
  } else if (node_count > 0) {
    auto [min_it, max_it] =
        std::minmax_element(node_raw_lane.begin(), node_raw_lane.end());
    int64_t min_lane = *min_it;
    uint64_t span =
        static_cast<uint64_t>(*max_it) - static_cast<uint64_t>(min_lane);
    if (span < 2 * uint64_t{node_count} + 1024) {
      std::vector<uint32_t> lane_of_offset(span + 1, kNoNode);
      for (uint32_t u = 0; u < node_count; ++u) {
        uint32_t& slot =
            lane_of_offset[static_cast<uint64_t>(node_raw_lane[u]) -
                           static_cast<uint64_t>(min_lane)];
        if (slot == kNoNode) {
          slot = num_lanes++;
        }
        lane_of_node[u] = slot;
      }
    } else {
      base::FlatHashMap<int64_t, uint32_t> lane_map;
      for (uint32_t u = 0; u < node_count; ++u) {
        auto it_and_ins = lane_map.Insert(node_raw_lane[u], num_lanes);
        if (it_and_ins.second) {
          ++num_lanes;
        }
        lane_of_node[u] = *it_and_ins.first;
      }
    }
  }

  using Seed = State::Seed;
  bool keep_seed_rows = !spec_.seed_retained_columns.empty();
  std::vector<Seed>& seeds = s.seeds;
  while (spec_.seeds->GetData(s.batch, *s.seeds_state)) {
    uint32_t seed_row_base = s.seed_rows.size();
    FlatColumnReader<int64_t> ids(s.batch, spec_.seed_column);
    std::optional<FlatColumnReader<int64_t>> ts_col;
    std::optional<FlatColumnReader<int64_t>> dur_col;
    if (spec_.seed_ts_column && spec_.seed_dur_column) {
      ts_col.emplace(s.batch, *spec_.seed_ts_column);
      dur_col.emplace(s.batch, *spec_.seed_dur_column);
    }
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t id;
      if (!ids.Read(row, &id)) {
        continue;
      }
      uint32_t u = find_node(id);
      if (u == kNoNode) {
        continue;
      }
      int64_t ts = node_ts[u];
      int64_t end = node_end[u];
      if (ts_col && dur_col) {
        int64_t s_ts = 0;
        int64_t s_dur = 0;
        if (!ts_col->Read(row, &s_ts) || !dur_col->Read(row, &s_dur) ||
            s_dur <= 0) {
          continue;
        }
        ts = s_ts;
        end = s_ts + s_dur;
      }
      if (ts < end) {
        seeds.push_back({u, ts, end, seed_row_base + row});
      }
    }
    if (keep_seed_rows) {
      s.retained.Project(s.batch, spec_.seed_retained_columns);
      RETURN_IF_ERROR(s.seed_rows.Append(s.retained));
    }
  }
  RETURN_IF_ERROR(spec_.seeds->status(*s.seeds_state));

  using LaneSpan = State::LaneSpan;

  std::vector<uint32_t>& lane_start = s.lane_start;
  lane_start.assign(num_lanes + 1, 0);
  for (uint32_t u = 0; u < node_count; ++u) {
    if (node_ts[u] < node_end[u]) {
      lane_start[lane_of_node[u] + 1]++;
    }
  }

  std::vector<LaneSpan> raw_edges;
  raw_edges.reserve(node_count);
  while (spec_.edges->GetData(s.batch, *s.edges_state)) {
    FlatColumnReader<int64_t> from(s.batch, spec_.source_column);
    FlatColumnReader<int64_t> to(s.batch, spec_.target_column);
    FlatColumnReader<int64_t> ts_col(s.batch, spec_.edge_ts_column);
    FlatColumnReader<int64_t> dur_col(s.batch, spec_.edge_dur_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t source;
      int64_t target;
      int64_t e_ts;
      int64_t e_dur;
      if (!from.Read(row, &source) || !to.Read(row, &target) ||
          !ts_col.Read(row, &e_ts) || !dur_col.Read(row, &e_dur) ||
          e_dur <= 0) {
        continue;
      }
      uint32_t src_node = find_node(source);
      uint32_t dst_node = find_node(target);
      if (src_node == kNoNode || dst_node == kNoNode) {
        continue;
      }
      // Waiting only reaches back in time, which also keeps a cycle in the
      // edges from being walked forever.
      if (dst_node != src_node && node_ts[dst_node] >= node_ts[src_node]) {
        continue;
      }
      int64_t e_end = dst_node == src_node
                          ? (e_ts + e_dur)
                          : std::min(e_ts + e_dur, node_end[dst_node]);
      if (e_ts < e_end) {
        raw_edges.push_back(
            {e_ts, e_end, src_node, dst_node == src_node ? kNoNode : dst_node});
        lane_start[lane_of_node[src_node] + 1]++;
      }
    }
  }
  RETURN_IF_ERROR(spec_.edges->status(*s.edges_state));

  if (seeds.empty()) {
    return base::OkStatus();
  }

  for (uint32_t l = 0; l < num_lanes; ++l) {
    lane_start[l + 1] += lane_start[l];
  }

  std::vector<LaneSpan>& spans = s.spans;
  spans.resize(lane_start[num_lanes]);
  std::vector<uint32_t> write_pos = lane_start;
  for (const LaneSpan& edge : raw_edges) {
    spans[write_pos[lane_of_node[edge.node]]++] = edge;
  }
  for (uint32_t u = 0; u < node_count; ++u) {
    if (node_ts[u] < node_end[u]) {
      spans[write_pos[lane_of_node[u]]++] = {node_ts[u], node_end[u], u,
                                             kNoNode};
    }
  }

  std::vector<int64_t>& prefix_max_end = s.prefix_max_end;
  prefix_max_end.resize(spans.size());
  std::vector<bool>& lane_disjoint = s.lane_disjoint;
  lane_disjoint.assign(num_lanes, true);
  std::vector<uint32_t>& node_last_span = s.node_last_span;
  node_last_span.assign(node_count, kNoNode);
  auto span_less = [](const LaneSpan& a, const LaneSpan& b) {
    if (a.ts != b.ts) {
      return a.ts < b.ts;
    }
    if ((a.dst_node == kNoNode) != (b.dst_node == kNoNode)) {
      return a.dst_node != kNoNode;
    }
    return a.end < b.end;
  };
  for (uint32_t l = 0; l < num_lanes; ++l) {
    uint32_t begin = lane_start[l];
    uint32_t end = lane_start[l + 1];
    if (begin == end) {
      continue;
    }
    if (!std::is_sorted(spans.begin() + begin, spans.begin() + end,
                        span_less)) {
      std::sort(spans.begin() + begin, spans.begin() + end, span_less);
    }
    bool disjoint = true;
    int64_t max_end = std::numeric_limits<int64_t>::min();
    for (uint32_t i = begin; i < end; ++i) {
      if (i > begin && spans[i - 1].end > spans[i].ts) {
        disjoint = false;
      }
      max_end = std::max(max_end, spans[i].end);
      prefix_max_end[i] = max_end;
      if (spans[i].dst_node == kNoNode) {
        node_last_span[spans[i].node] = i;
      }
    }
    lane_disjoint[l] = disjoint;
  }

  if (keep_rows) {
    s.gather_rows.reserve(kMaxBatchRows);
  }
  if (keep_seed_rows) {
    s.seed_gather_rows.reserve(kMaxBatchRows);
  }
  return base::OkStatus();
}

bool GraphTree::FillCriticalPathBatch(RowBatch& out, State& s) const {
  using StackFrame = State::StackFrame;
  using EmittedRow = State::EmittedRow;
  using LaneSpan = State::LaneSpan;

  out.Reset();
  if (s.root_row_idx == s.root_rows.size() && s.seed_idx == s.seeds.size()) {
    return false;
  }

  bool keep_rows = !spec_.retained_columns.empty();
  bool keep_seed_rows = !spec_.seed_retained_columns.empty();
  auto node_count = static_cast<uint32_t>(s.id_of_node.size());
  const auto& node_ts = s.node_ts;
  const auto& node_end = s.node_end;
  const auto& lane_of_node = s.lane_of_node;
  const auto& lane_start = s.lane_start;
  const auto& spans = s.spans;
  const auto& prefix_max_end = s.prefix_max_end;
  const auto& lane_disjoint = s.lane_disjoint;
  const auto& node_last_span = s.node_last_span;

  ColumnBuffer ids = s.context->TakeBuffer();
  ColumnBuffer tss = s.context->TakeBuffer();
  ColumnBuffer durs = s.context->TakeBuffer();
  ColumnBuffer root_ids = s.context->TakeBuffer();
  ColumnBuffer depths = s.context->TakeBuffer();
  ColumnBuffer parent_ids = s.context->TakeBuffer();
  ColumnBuffer groups = s.context->TakeBuffer();

  auto* out_id = ids.chunk().Values<int64_t>();
  auto* out_ts = tss.chunk().Values<int64_t>();
  auto* out_dur = durs.chunk().Values<int64_t>();
  auto* out_root_id = root_ids.chunk().Values<int64_t>();
  auto* out_depth = depths.chunk().Values<int64_t>();
  ColumnChunk& parent_id_chunk = parent_ids.chunk();
  auto* out_parent_id = parent_id_chunk.Values<int64_t>();
  parent_id_chunk.validity.resize(kMaxBatchRows);
  parent_id_chunk.validity.FillBits(0, kMaxBatchRows, true);
  auto* out_group = groups.chunk().Values<uint32_t>();
  if (keep_rows) {
    s.gather_rows.clear();
  }
  if (keep_seed_rows) {
    s.seed_gather_rows.clear();
  }

  auto row_less = [](const EmittedRow& a, const EmittedRow& b) {
    if (a.ts != b.ts) {
      return a.ts < b.ts;
    }
    return a.depth < b.depth;
  };

  uint32_t count = 0;
  while (count < kMaxBatchRows) {
    while (s.root_row_idx < s.root_rows.size() && count < kMaxBatchRows) {
      const EmittedRow& r = s.root_rows[s.root_row_idx++];
      if (keep_rows) {
        s.gather_rows.push_back(r.node);
      }
      if (keep_seed_rows) {
        s.seed_gather_rows.push_back(s.cur_seed_row);
      }
      out_id[count] = s.id_of_node[r.node];
      out_ts[count] = r.ts;
      out_dur[count] = r.dur;
      out_root_id[count] = s.cur_root_id;
      out_depth[count] = static_cast<int64_t>(r.depth);
      if (r.parent_id < 0) {
        out_parent_id[count] = 0;
        parent_id_chunk.validity.change(count, false);
      } else {
        out_parent_id[count] = r.parent_id;
      }
      out_group[count] = s.cur_group;
      ++count;
    }
    if (count == kMaxBatchRows || s.seed_idx == s.seeds.size()) {
      break;
    }

    const State::Seed& seed = s.seeds[s.seed_idx++];
    s.root_rows.clear();
    s.root_row_idx = 0;
    s.stack.clear();
    s.stack.push_back(
        {lane_of_node[seed.node], seed.node, seed.ts, seed.end, 0, -1});

    while (!s.stack.empty()) {
      StackFrame f = s.stack.back();
      s.stack.pop_back();
      if (f.w_start >= f.w_end) {
        continue;
      }
      uint32_t begin = lane_start[f.lane];
      uint32_t end = lane_start[f.lane + 1];
      if (begin == end) {
        continue;
      }

      if (lane_disjoint[f.lane] && f.target_node != kNoNode &&
          node_last_span[f.target_node] != kNoNode &&
          f.w_end <= node_end[f.target_node]) {
        uint32_t hi = node_last_span[f.target_node];
        while (hi > begin && spans[hi].ts >= f.w_end) {
          --hi;
        }
        if (spans[hi].ts >= f.w_end || spans[hi].end <= f.w_start) {
          continue;
        }
        uint32_t lo = hi;
        uint32_t steps = 0;
        while (lo > begin && spans[lo - 1].end > f.w_start) {
          --lo;
          if (++steps == 8 && lo > begin && spans[lo - 1].end > f.w_start) {
            auto it = std::upper_bound(
                spans.begin() + begin, spans.begin() + lo, f.w_start,
                [](int64_t val, const LaneSpan& sp) { return val < sp.end; });
            lo = static_cast<uint32_t>(it - spans.begin());
            break;
          }
        }
        for (uint32_t i = lo; i <= hi; ++i) {
          const LaneSpan& sp = spans[i];
          int64_t c_start = std::max(f.w_start, sp.ts);
          int64_t c_end = std::min(f.w_end, sp.end);
          if (sp.dst_node == kNoNode) {
            s.root_rows.push_back(
                {c_start, c_end - c_start, f.parent_id, sp.node, f.depth});
          } else if (f.depth + 1 < node_count) {
            uint32_t dst = sp.dst_node;
            s.stack.push_back({lane_of_node[dst], dst, c_start, c_end,
                               f.depth + 1, s.id_of_node[sp.node]});
          }
        }
      } else {
        auto it = std::upper_bound(prefix_max_end.begin() + begin,
                                   prefix_max_end.begin() + end, f.w_start);
        auto first = static_cast<uint32_t>(it - prefix_max_end.begin());
        for (uint32_t i = first; i < end && spans[i].ts < f.w_end; ++i) {
          const LaneSpan& sp = spans[i];
          if (sp.end <= f.w_start) {
            continue;
          }
          if (f.target_node != kNoNode && sp.node != f.target_node &&
              node_ts[sp.node] >= node_ts[f.target_node]) {
            continue;
          }
          int64_t c_start = std::max(f.w_start, sp.ts);
          int64_t c_end = std::min(f.w_end, sp.end);
          if (c_start >= c_end) {
            continue;
          }
          if (sp.dst_node == kNoNode) {
            s.root_rows.push_back(
                {c_start, c_end - c_start, f.parent_id, sp.node, f.depth});
          } else if (f.depth + 1 < node_count) {
            uint32_t dst = sp.dst_node;
            s.stack.push_back({lane_of_node[dst], dst, c_start, c_end,
                               f.depth + 1, s.id_of_node[sp.node]});
          }
        }
      }
    }

    if (s.root_rows.empty()) {
      continue;
    }
    if (s.root_rows.size() > 1) {
      std::sort(s.root_rows.begin(), s.root_rows.end(), row_less);
      int64_t cov_end = s.root_rows.front().ts;
      size_t orig_size = s.root_rows.size();
      bool has_gaps = false;
      for (size_t i = 0; i < orig_size; ++i) {
        if (s.root_rows[i].ts > cov_end) {
          has_gaps = true;
        }
        cov_end = std::max(cov_end, s.root_rows[i].ts + s.root_rows[i].dur);
      }
      if (PERFETTO_UNLIKELY(has_gaps)) {
        cov_end = s.root_rows.front().ts;
        for (size_t i = 0; i < orig_size; ++i) {
          const EmittedRow& r = s.root_rows[i];
          if (r.ts > cov_end) {
            s.root_rows.push_back({cov_end, r.ts - cov_end, -1, seed.node, 0});
            cov_end = r.ts;
          }
          cov_end = std::max(cov_end, r.ts + r.dur);
        }
        std::inplace_merge(
            s.root_rows.begin(),
            s.root_rows.begin() + static_cast<ptrdiff_t>(orig_size),
            s.root_rows.end(), row_less);
      }
    }
    s.cur_root_id = s.id_of_node[seed.node];
    s.cur_seed_row = seed.seed_row;
    s.cur_group = s.group_num++;
  }

  if (count == 0) {
    return false;
  }
  if (keep_rows) {
    const uint32_t* rows = s.gather_rows.data();
    s.rows.Gather(&s.gathered, {rows, rows + count}, *s.context);
    for (uint32_t c = 0; c < s.gathered.column_count(); ++c) {
      out.AddColumn(s.gathered.column(c), s.gathered.buffer(c));
    }
  }
  if (keep_seed_rows) {
    const uint32_t* rows = s.seed_gather_rows.data();
    s.seed_rows.Gather(&s.gathered, {rows, rows + count}, *s.context);
    for (uint32_t c = 0; c < s.gathered.column_count(); ++c) {
      out.AddColumn(s.gathered.column(c), s.gathered.buffer(c));
    }
  }
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, out_id),
                std::move(ids));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, out_ts),
                std::move(tss));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, out_dur),
                std::move(durs));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, out_root_id),
                std::move(root_ids));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, out_depth),
                std::move(depths));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, out_parent_id,
                                      &parent_id_chunk.validity),
                std::move(parent_ids));
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, out_group),
                std::move(groups));
  out.SetRowCount(count);
  return true;
}

base::Status GraphTree::Build(State& s) const {
  if (spec_.kind == GraphTreeSpec::Kind::kCriticalPath) {
    return BuildCriticalPath(s);
  }
  const char* name = Name(spec_.kind);

  while (spec_.nodes->GetData(s.batch, *s.nodes_state)) {
    FlatColumnReader<int64_t> ids(s.batch, spec_.node_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t id;
      if (!ids.Read(row, &id)) {
        return base::ErrStatus("%s: a node has no id", name);
      }
      s.id_of_node.push_back(id);
    }
    s.retained.Project(s.batch, spec_.retained_columns);
    RETURN_IF_ERROR(s.rows.Append(s.retained));
  }
  RETURN_IF_ERROR(spec_.nodes->status(*s.nodes_state));

  auto node_count = static_cast<uint32_t>(s.id_of_node.size());
  NodeIndex index;
  if (std::optional<int64_t> twice = index.Build(s.id_of_node)) {
    return base::ErrStatus("%s: two nodes have the id %" PRId64, name, *twice);
  }

  std::vector<uint32_t> seeds;
  while (spec_.seeds->GetData(s.batch, *s.seeds_state)) {
    FlatColumnReader<int64_t> ids(s.batch, spec_.seed_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t id;
      if (!ids.Read(row, &id)) {
        continue;
      }
      if (uint32_t node = index.Find(id); node != kNoNode) {
        seeds.push_back(node);
      }
    }
  }
  RETURN_IF_ERROR(spec_.seeds->status(*s.seeds_state));

  // A dominator tree has one root, so it grows from an extra node with an
  // edge to every seed.
  bool dominator = spec_.kind == GraphTreeSpec::Kind::kDominator;
  uint32_t graph_nodes = node_count + (dominator ? 1 : 0);
  std::vector<uint32_t> sources;
  std::vector<uint32_t> targets;
  while (spec_.edges->GetData(s.batch, *s.edges_state)) {
    FlatColumnReader<int64_t> from(s.batch, spec_.source_column);
    FlatColumnReader<int64_t> to(s.batch, spec_.target_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t source;
      int64_t target;
      if (!from.Read(row, &source) || !to.Read(row, &target)) {
        continue;
      }
      uint32_t source_node = index.Find(source);
      uint32_t target_node = index.Find(target);
      if (source_node != kNoNode && target_node != kNoNode) {
        sources.push_back(source_node);
        targets.push_back(target_node);
      }
    }
  }
  RETURN_IF_ERROR(spec_.edges->status(*s.edges_state));
  if (dominator) {
    for (uint32_t seed : seeds) {
      sources.push_back(node_count);
      targets.push_back(seed);
    }
  }

  util::Digraph graph = util::Digraph::FromEdges(graph_nodes, sources, targets);

  switch (spec_.kind) {
    case GraphTreeSpec::Kind::kBfs:
      s.tree = util::BreadthFirstSearch(graph, seeds);
      break;
    case GraphTreeSpec::Kind::kDfs:
      s.tree = util::DepthFirstSearch(graph, seeds);
      break;
    case GraphTreeSpec::Kind::kDominator:
      s.tree = util::DominatorTree(graph, node_count);
      s.tree.order.erase(s.tree.order.begin());
      s.tree.parent.pop_back();
      for (uint32_t node : s.tree.order) {
        if (s.tree.parent[node] == node_count) {
          s.tree.parent[node] = kNoParent;
        }
      }
      break;
    case GraphTreeSpec::Kind::kCriticalPath:
      PERFETTO_FATAL("Unreachable");
  }

  s.position.assign(node_count, kNoNode);
  for (uint32_t i = 0; i < s.tree.order.size(); ++i) {
    s.position[s.tree.order[i]] = i;
  }
  return base::OkStatus();
}

bool GraphTree::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (!s.computed) {
    s.status = Build(s);
    if (!s.status.ok()) {
      return false;
    }
    s.computed = true;
  }

  if (spec_.kind == GraphTreeSpec::Kind::kCriticalPath) {
    return FillCriticalPathBatch(out, s);
  }

  auto total = static_cast<uint32_t>(s.tree.order.size());
  uint32_t count = std::min(total - s.served, kMaxBatchRows);
  out.Reset();
  if (count == 0) {
    return false;
  }

  ColumnBuffer parent_ids = s.context->TakeBuffer();
  ColumnBuffer numbers = s.context->TakeBuffer();
  ColumnBuffer parent_numbers = s.context->TakeBuffer();
  ColumnChunk& parent_id_chunk = parent_ids.chunk();
  auto* parent_id = parent_id_chunk.Values<int64_t>();
  parent_id_chunk.validity.resize(kMaxBatchRows);
  auto* number = numbers.chunk().Values<uint32_t>();
  auto* parent_number = parent_numbers.chunk().Values<uint32_t>();
  s.gather_rows.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = s.tree.order[s.served + i];
    uint32_t parent = s.tree.parent[node];
    s.gather_rows[i] = node;
    number[i] = s.served + i;
    bool root = parent == kNoParent;
    parent_id[i] = root ? 0 : s.id_of_node[parent];
    parent_id_chunk.validity.change(i, !root);
    parent_number[i] = root ? kNoNode : s.position[parent];
  }
  s.served += count;

  const uint32_t* rows = s.gather_rows.data();
  s.rows.Gather(&s.gathered, {rows, rows + count}, *s.context);
  for (uint32_t c = 0; c < s.gathered.column_count(); ++c) {
    out.AddColumn(s.gathered.column(c), s.gathered.buffer(c));
  }
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, parent_id,
                                      &parent_id_chunk.validity),
                std::move(parent_ids));
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, number),
                std::move(numbers));
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parent_number),
                std::move(parent_numbers));
  out.SetRowCount(count);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
