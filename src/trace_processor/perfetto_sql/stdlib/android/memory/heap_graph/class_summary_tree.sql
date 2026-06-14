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
-- distributed ON an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

-- Table containing all the Android heap graphs in the trace converted to a
-- shortest-path tree and then aggregated by class name.
--
-- This table contains a "flamegraph-like" representation of the contents of the
-- heap graph.
CREATE PERFETTO PIPELINE android_heap_graph_class_summary_tree(
  -- The timestamp the heap graph was dumped at.
  graph_sample_ts TIMESTAMP,
  -- The upid of the process.
  upid JOINID(process.id),
  -- The id of the node in the class tree.
  id LONG,
  -- The parent id of the node in the class tree or NULL if this is the root.
  parent_id LONG,
  -- The name of the class.
  name STRING,
  -- A string describing the type of Java root if this node is a root or NULL
  -- if this node is not a root.
  root_type STRING,
  -- The count of objects with the same class name and the same path to the
  -- root.
  self_count LONG,
  -- The size of objects with the same class name and the same path to the
  -- root.
  self_size LONG,
  -- The sum of `self_count` of this node and all descendants of this node.
  cumulative_count LONG,
  -- The sum of `self_size` of this node and all descendants of this node.
  cumulative_size LONG
)
MATERIALIZED AS
-- _graph_aggregating_scan! seeded at the leaves with self_count/self_size and
-- summing up to the root is exactly a subtree sum over the class tree.
FROM _heap_graph_class_tree
|> TREE ACCUMULATE UP
   SUM(self_count) AS cumulative_count,
   SUM(self_size) AS cumulative_size
|> SELECT
  graph_sample_ts,
  upid,
  id,
  parent_id,
  name,
  root_type,
  self_count,
  self_size,
  cumulative_count,
  cumulative_size;
