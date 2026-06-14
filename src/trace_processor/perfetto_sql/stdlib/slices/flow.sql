--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Computes the "reachable" set of slices from the |flow| table, starting from
-- slice ids specified in |source_table|.
--
-- The depth-first spanning tree over the flow graph from the seeds; the per-node
-- origin seed (|root_node_id|) — which the old weight-bounded-dfs intrinsic carried
-- explicitly — is recovered with TREE ACCUMULATE DOWN FIRST(node_id), i.e. the root
-- of each node's root-to-node path.
CREATE PERFETTO MACRO _slice_following_flow(
  -- A table/view/subquery corresponding to the nodes to start the reachability search.
  -- This table must have a uint32 "id" column.
  source_table TableOrSubquery
)
-- The returned table has the schema (root_node_id, node_id LONG, parent_node_id LONG).
-- |root_node_id| is the seed under which this node was encountered, |node_id| the
-- node from the input graph, and |parent_node_id| its DFS predecessor.
RETURNS Pipeline
AS (
  GRAPH DFS TREE
    NODES (FROM flow |> SELECT slice_in AS node_id)
    EDGES (FROM flow |> SELECT slice_out AS source_node_id, slice_in AS dest_node_id)
    FROM (
      FROM flow
      |> JOIN $source_table AS source ON flow.slice_out = source.id
      |> SELECT slice_out AS node_id
    )
  |> TREE ACCUMULATE DOWN FIRST(node_id) AS root_node_id
);
