--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE callstacks.stack_profile;

-- Builds a flamegraph callstack tree from heap profile allocations, summing
-- the input per callsite and attaching the results as self_* metrics.
--
-- size/count should be raw signed values (frees are negative). The net is
-- clamped to 0 after summing, as flamegraphs reject negative values. Don't
-- clamp them per row, as that discards frees. alloc_size/alloc_count are gross
-- allocations, so pass max(size, 0) / max(count, 0) for them.
CREATE PERFETTO MACRO _android_heap_profile_callstacks_for_allocations(
  -- Rows of (callsite_id, size, count, alloc_size, alloc_count).
  allocations TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH
    metrics AS MATERIALIZED (
      SELECT
        callsite_id,
        max(sum(size), 0) AS self_size,
        max(sum(count), 0) AS self_count,
        sum(alloc_size) AS self_alloc_size,
        sum(alloc_count) AS self_alloc_count
      FROM $allocations
      GROUP BY
        callsite_id
    )
  SELECT
    c.id,
    c.parent_id,
    c.name,
    c.mapping_name,
    c.source_file,
    c.line_number,
    coalesce(m.self_size, 0) AS self_size,
    coalesce(m.self_count, 0) AS self_count,
    coalesce(m.self_alloc_size, 0) AS self_alloc_size,
    coalesce(m.self_alloc_count, 0) AS self_alloc_count
  FROM _callstacks_for_stack_profile_samples!(metrics) AS c
  LEFT JOIN metrics AS m
    USING (callsite_id)
);
