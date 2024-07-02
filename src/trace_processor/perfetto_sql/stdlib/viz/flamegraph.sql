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

include perfetto module graphs.scan;

CREATE PERFETTO MACRO _viz_flamegraph_prepare_filter(
  tab TableOrSubquery,
  show_frame Expr,
  hide_frame Expr,
  show_stack Expr,
  hide_stack Expr,
  impossible_stack_bits Expr
)
RETURNS TableOrSubquery
AS (
  SELECT
    *,
    IIF($hide_stack, $impossible_stack_bits, $show_stack) AS stackBits,
    $show_frame AND NOT $hide_frame AS showFrame
  FROM $tab
);

CREATE PERFETTO MACRO _viz_flamegraph_filter_and_hash(
  tab TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT id, hash, parentHash, depth, stackBits
  FROM _graph_scan!(
    (
      SELECT parentId AS source_node_id, id AS dest_node_id
      FROM $tab
      WHERE parentId IS NOT NULL
    ),
    (
      select
        id,
        IIF(showFrame, HASH(name), 0) AS hash,
        IIF(showFrame, 0, NULL) AS parentHash,
        IIF(showFrame, 0, -1) AS depth,
        IIF(showFrame, stackBits, 0) AS stackBits
      FROM $tab
      WHERE parentId IS NULL
    ),
    (hash, parentHash, depth, stackBits),
    (
      select
        t.id as id,
        IIF(x.showFrame, HASH(t.hash, name), t.hash) AS hash,
        IIF(x.showFrame, t.hash, null) AS parentHash,
        IIF(x.showFrame, t.depth + 1, t.depth) AS depth,
        IIF(
          x.showFrame,
          (t.stackBits | x.stackBits),
          t.stackBits
        ) AS stackBits
      FROM $table t
      JOIN $tab x USING (id)
    )
  ) g
  WHERE parentHash IS NOT NULL
  ORDER BY hash
);

CREATE PERFETTO MACRO _viz_flamegraph_merge_hashes(
  tab TableOrSubquery,
  source TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    c._auto_id AS id,
    (
      SELECT p._auto_id
      FROM $tab p
      WHERE p.hash = c.parentHash
      LIMIT 1
    ) AS parentId,
    c.depth,
    c.stackBits,
    s.name,
    SUM(s.value) AS value
  FROM $tab c
  JOIN $source s USING (id)
  GROUP BY hash
);

CREATE PERFETTO MACRO _viz_flamegraph_accumulate(
  tab TableOrSubquery,
  allStackBits Expr
)
RETURNS TableOrSubquery
AS (
  SELECT id, cumulativeValue
  FROM _graph_scan!(
    (
      SELECT id AS source_node_id, parentId AS dest_node_id
      FROM $tab
      WHERE parentId IS NOT NULL
    ),
    (
      SELECT t.id AS id, t.value AS cumulativeValue
      FROM $tab t
      LEFT JOIN $tab c ON t.id = c.parentId
      WHERE c.id IS NULL AND t.stackBits = $allStackBits
    ),
    (cumulativeValue),
    (
      SELECT
        x.id,
        x.childValue + IIF(
          t.stackBits = $allStackBits,
          t.value,
          0
        ) AS cumulativeValue
      FROM (
        SELECT id, SUM(cumulativeValue) AS childValue
        FROM $table
        GROUP BY 1
      ) x
      JOIN $tab t USING (id)
    )
  )
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_local_layout(
  acc TableOrSubquery,
  tab TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT id, xEnd - cumulativeValue as xStart, xEnd
  FROM (
    SELECT
      b.id,
      b.cumulativeValue,
      SUM(b.cumulativeValue) OVER win AS xEnd
    FROM $acc b
    JOIN $tab s USING (id)
    WINDOW win AS (
      PARTITION BY s.parentId
      ORDER BY b.cumulativeValue DESC
      ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )
  )
);

CREATE PERFETTO MACRO _viz_flamegraph_global_layout(
  layout TableOrSubquery,
  acc TableOrSubquery,
  tab TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  select
    s.id,
    ifnull(t.parentId, -1) as parentId,
    t.depth,
    t.name,
    t.value as selfValue,
    b.cumulativeValue,
    s.xStart,
    s.xEnd
  from _graph_scan!(
    (
      select parentId as source_node_id, id as dest_node_id
      from $tab
      where parentId is not null
    ),
    (
      select b.id as id, w.xStart, w.xEnd
      from $acc b
      join $tab t using (id)
      join $layout w using (id)
      where t.parentId is null
    ),
    (xStart, xEnd),
    (
      select
        t.id,
        t.xStart + w.xStart as xStart,
        t.xStart + w.xEnd as xEnd
      from $table t
      join $layout w using (id)
    )
  ) s
  join $tab t using (id)
  join $acc b using (id)
  order by depth, xStart
);
