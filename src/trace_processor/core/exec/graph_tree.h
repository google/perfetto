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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_GRAPH_TREE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_GRAPH_TREE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

struct GraphTreeSpec {
  enum class Kind : uint8_t { kBfs, kDfs, kDominator, kCriticalPath };
  Kind kind = Kind::kBfs;

  // The nodes: one row each, with a distinct Int64 id. The retained columns
  // are what each node's row out carries.
  const Source* nodes = nullptr;
  uint32_t node_column = 0;
  // CRITICAL PATH only: the node's interval and its lane. A lane equal to
  // `node_column` puts every node on a lane of its own.
  uint32_t node_ts_column = 0;
  uint32_t node_dur_column = 0;
  uint32_t node_lane_column = 0;
  std::vector<uint32_t> retained_columns;

  // The edges, from the node in `source_column` to the node in
  // `target_column`, both Int64.
  const Source* edges = nullptr;
  uint32_t source_column = 0;
  uint32_t target_column = 0;
  // CRITICAL PATH only: when the edge holds.
  uint32_t edge_ts_column = 0;
  uint32_t edge_dur_column = 0;

  // The nodes the tree grows from, by Int64 id.
  const Source* seeds = nullptr;
  uint32_t seed_column = 0;
  // CRITICAL PATH only: the window each seed is walked over. Without them, the
  // seed node's own interval.
  std::optional<uint32_t> seed_ts_column;
  std::optional<uint32_t> seed_dur_column;
  // CRITICAL PATH only: columns of `seeds` passed on to the rows out.
  std::vector<uint32_t> seed_retained_columns;
};

// GRAPH BFS | DFS | DOMINATOR TREE: the tree a graph reduces to over the nodes
// reachable from the seeds.
//
// Every input is read in full first. A node without an id, or two nodes with
// the same id, is an error. An edge or seed which names no node, or holds no
// value, is ignored, so the nodes can be a subset of the graph the edges
// describe. The seeds, and each node's edges, are taken in the order they
// arrive, which is how a caller chooses between equally good parents.
//
//  - BFS: every seed is a root. A node's parent is the first node searched
//    which has an edge to it.
//  - DFS: a search starts from each seed in turn which is not yet reached, so
//    a seed reached from an earlier one is not a root. A node's parent is the
//    node it was reached from.
//  - DOMINATOR: every seed is a root. A node's parent is its immediate
//    dominator, as though one more node had an edge to every seed.
//
// One row is emitted per node in the tree, parents before their children: the
// retained columns of the node's row, then the parent's id as an Int64, null
// for a root, then two Uint32 columns numbering the rows out from zero, of the
// row and of its parent's row, kNoNode for a root. The numbers are what the
// tree operators read, as though from TreeNumberNodes.
//
// GRAPH CRITICAL PATH: explains each seed's window by following who it waited
// on, as time slices. Every node is an interval on a lane, and every edge says
// that over [ts, ts + dur) its source waits on its target, clipped to the
// target's end. An edge from a node to itself makes the node stand for its own
// wait. Walking a window over a lane clips it to each node and edge there: a
// node emits a row for the slice it covers, and an edge walks the clipped slice
// over its target's lane one level deeper. Time only runs backwards: an edge is
// ignored unless its target started before its source, and on the target's
// lane only nodes which started no later than the target are looked at, so
// every walk ends. Rows out are the retained columns, the slice's ts and dur,
// the seed's id, the depth, the id of the node whose edge led there (null at
// depth 0), and a Uint32 group number per seed. A node appears once per slice,
// so the rows are not a tree the tree operators can read. Each seed's rows are
// sorted by (ts, depth), so they can feed INTERVAL FLATTEN PER root_id without
// a sort. Rows are produced a batch at a time, a seed at a time.
class GraphTree : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // Each input's source must outlive this.
  explicit GraphTree(GraphTreeSpec);
  ~GraphTree() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool GetData(RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

  struct State;

  // Reads every input and builds the tree, in the order its rows go out.
  base::Status Build(State&) const;
  base::Status BuildCriticalPath(State&) const;
  bool FillCriticalPathBatch(RowBatch&, State&) const;

  GraphTreeSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_GRAPH_TREE_H_
