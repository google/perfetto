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

CREATE PERFETTO TABLE _counter_track_summary AS
WITH distinct_ids AS (
  SELECT DISTINCT track_id as id
  FROM counter
)
SELECT
  distinct_ids.id,
  COALESCE(SUM(
    args.key = 'upid'
    OR args.key = 'utid'
    OR args.key = 'cpu'
    OR args.key = 'gpu'
  ), 0) = 0 AS is_legacy_global
FROM distinct_ids
JOIN counter_track USING (id)
LEFT JOIN args ON counter_track.dimension_arg_set_id = args.arg_set_id
GROUP BY distinct_ids.id;
