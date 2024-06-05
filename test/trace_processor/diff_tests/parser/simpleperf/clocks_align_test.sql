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
--

CREATE PERFETTO VIEW perf_sample_in(ts INT, dur INT)
AS
SELECT ts, 0 AS dur FROM perf_sample;

CREATE VIRTUAL TABLE span
USING
  SPAN_JOIN(perf_sample_in, slice PARTITIONED depth);

CREATE PERFETTO TABLE slice_stack
AS
WITH
  tmp AS (
    SELECT
      ts,
      parent_stack_id,
      string_AGG(IIF(name = 'Main loop', 'main', name), ',')
        OVER (
          PARTITION BY ts
          ORDER BY depth ASC
          RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
        ) AS stack
    FROM span
  )
SELECT ts, stack FROM tmp WHERE parent_stack_id = 0 ORDER BY TS ASC;

CREATE PERFETTO TABLE perf_stack
AS
WITH
  symbol AS (
    SELECT
      id,
      symbol_set_id,
      replace(replace(name, '(anonymous namespace)::', ''), '()', '') AS name
    FROM stack_profile_symbol
  ),
  symbol_agg AS (
    SELECT
      id,
      symbol_set_id,
      string_agg(name, ',')
        OVER (
          PARTITION BY symbol_set_id
          ORDER BY id DESC
          RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
        ) AS name
    FROM symbol
    WHERE name IN ('main', 'A', 'B', 'C', 'D', 'E')
  ),
  inline AS (
    SELECT symbol_set_id, name FROM symbol_agg WHERE id = symbol_set_id
  ),
  frame AS (
    SELECT f.id AS frame_id, i.name
    FROM STACK_PROFILE_FRAME f, inline i
    USING (symbol_set_id)
  ),
  child AS (
    SELECT
      s.ts,
      spc.id,
      spc.parent_id,
      name
    FROM perf_sample s, stack_profile_callsite spc
    ON (s.callsite_id = spc.id),
    frame USING (frame_id)
    UNION ALL
    SELECT
      child.ts,
      parent.id,
      parent.parent_id,
      COALESCE(f.name || ',', '') || child.name AS name
    FROM child, stack_profile_callsite parent
    ON (child.parent_id = parent.id)
    LEFT JOIN frame f
      USING (frame_id)
  )
SELECT ts, name AS stack FROM child WHERE parent_id IS NULL ORDER BY ts ASC;

SELECT COUNT(*) AS misaligned_count
FROM slice_stack s
FULL JOIN perf_stack p
  USING (ts)
WHERE s.stack <> p.stack;
