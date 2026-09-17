--
-- Copyright 2026 The Android Open Source Project
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

-- Sample counts for every callsite on the stack of any of the given samples.
--
-- `self_count` is the number of samples whose leaf callsite is the row's
-- callsite. `total_count` additionally includes samples whose stack passes
-- through the callsite, i.e. samples taken in one of its callees.
CREATE PERFETTO MACRO _callsite_sample_counts(
  -- Subquery with a `callsite_id` column and one row per sample.
  samples TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH RECURSIVE
    metrics AS MATERIALIZED (
      SELECT
        callsite_id,
        count() AS self_count
      FROM $samples
      WHERE
        callsite_id IS NOT NULL
      GROUP BY
        callsite_id
    ),
    -- Every (ancestor or self, sampled callsite) pair.
    ancestors(callsite_id, sample_callsite_id) AS (
      SELECT
        callsite_id,
        callsite_id
      FROM metrics
      UNION ALL
      SELECT
        c.parent_id,
        a.sample_callsite_id
      FROM ancestors AS a
      JOIN stack_profile_callsite AS c
        ON c.id = a.callsite_id
      WHERE
        c.parent_id IS NOT NULL
    )
  SELECT
    a.callsite_id,
    sum(iif(a.callsite_id = m.callsite_id, m.self_count, 0)) AS self_count,
    sum(m.self_count) AS total_count
  FROM ancestors AS a
  JOIN metrics AS m
    ON m.callsite_id = a.sample_callsite_id
  GROUP BY
    a.callsite_id
);

-- Sample counts per instruction address.
--
-- For the leaf frame of a sample the address is the sampled program counter.
-- For every other frame it is the return address of the call the frame was
-- executing, so `total_count` on a call instruction includes the samples
-- taken inside the callee.
CREATE PERFETTO MACRO _sample_counts_by_address(
  -- Subquery with a `callsite_id` column and one row per sample.
  samples TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    f.mapping AS mapping_id,
    m.name AS mapping_name,
    m.build_id,
    f.rel_pc,
    sum(c.self_count) AS self_count,
    sum(c.total_count) AS total_count
  FROM _callsite_sample_counts!($samples) AS c
  JOIN stack_profile_callsite AS sc
    ON sc.id = c.callsite_id
  JOIN stack_profile_frame AS f
    ON f.id = sc.frame_id
  JOIN stack_profile_mapping AS m
    ON m.id = f.mapping
  GROUP BY
    f.mapping,
    f.rel_pc
);

-- Sample counts per source line, attributed through the innermost symbol of
-- each sampled frame. Frames without line information are not included.
CREATE PERFETTO MACRO _sample_counts_by_source_line(
  -- Subquery with a `callsite_id` column and one row per sample.
  samples TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    s.source_file,
    s.line_number,
    sum(c.self_count) AS self_count,
    sum(c.total_count) AS total_count
  FROM _callsite_sample_counts!($samples) AS c
  JOIN stack_profile_callsite AS sc
    ON sc.id = c.callsite_id
  JOIN stack_profile_frame AS f
    ON f.id = sc.frame_id
  -- The innermost inlined symbol of a frame is the one whose id equals the
  -- frame's symbol_set_id.
  JOIN stack_profile_symbol AS s
    ON s.id = f.symbol_set_id
  WHERE
    s.source_file IS NOT NULL AND s.line_number IS NOT NULL
  GROUP BY
    s.source_file,
    s.line_number
);

-- The bundled disassembly function containing an address in a mapping, or
-- NULL if no disassembly was bundled for it.
CREATE PERFETTO FUNCTION _disassembly_function_for_address(
  -- Id of the stack_profile_mapping the address belongs to.
  mapping_id LONG,
  -- Address relative to the start of the mapping.
  rel_pc LONG
)
-- Id in disassembly_function.
RETURNS LONG
AS
SELECT df.id
FROM disassembly_function AS df
JOIN stack_profile_mapping AS m
  ON m.id = $mapping_id
WHERE
  iif(df.build_id IS NOT NULL, df.build_id = m.build_id, df.path = m.name)
  AND $rel_pc >= df.start_rel_pc
  AND $rel_pc < df.start_rel_pc + df.size
LIMIT 1;

-- The bundled disassembly of a function, in address order, with the sample
-- counts of each instruction.
CREATE PERFETTO MACRO _annotated_disassembly(
  -- Subquery with a `callsite_id` column and one row per sample.
  samples TableOrSubquery,
  -- Id in disassembly_function.
  function_id Expr
)
RETURNS TableOrSubquery
AS (
  WITH
    fn AS (
      SELECT
        *
      FROM disassembly_function
      WHERE
        id = $function_id
    ),
    counts AS (
      SELECT
        a.rel_pc,
        sum(a.self_count) AS self_count,
        sum(a.total_count) AS total_count
      FROM _sample_counts_by_address!($samples) AS a
      JOIN fn
        ON iif(fn.build_id IS NOT NULL, a.build_id = fn.build_id, a.mapping_name = fn.path)
      GROUP BY
        a.rel_pc
    )
  SELECT
    i.id AS instruction_id,
    i.rel_pc,
    i.bytes,
    i.text,
    i.target_rel_pc,
    i.target_symbol,
    i.source_file,
    i.line_number,
    coalesce(c.self_count, 0) AS self_count,
    coalesce(c.total_count, 0) AS total_count
  FROM disassembly_instruction AS i
  LEFT JOIN counts AS c
    USING (rel_pc)
  WHERE
    i.function_id = $function_id
  ORDER BY
    i.rel_pc
);
