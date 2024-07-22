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

CREATE PERFETTO MACRO _android_heap_profile_callstacks_for_allocations(
  allocations TableOrSubquery
)
RETURNS TableOrSubquery
AS
(
  WITH metrics AS MATERIALIZED (
    SELECT
    callsite_id,
    SUM(size) AS self_size,
    SUM(count) AS self_count,
    SUM(alloc_size) AS self_alloc_size,
    SUM(alloc_count) AS self_alloc_count
    FROM $allocations
    GROUP BY callsite_id
  )
  SELECT
    c.id,
    c.parent_id,
    c.name,
    c.mapping_name,
    c.source_file,
    c.line_number,
    IFNULL(m.self_size, 0) AS self_size,
    IFNULL(m.self_count, 0) AS self_count,
    IFNULL(m.self_alloc_size, 0) AS self_alloc_size,
    IFNULL(m.self_alloc_count, 0) AS self_alloc_count
  FROM _callstacks_for_stack_profile_samples!(metrics) c
  LEFT JOIN metrics m USING (callsite_id)
);
