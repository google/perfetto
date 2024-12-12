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

CREATE PERFETTO TABLE _slice_track_summary AS
WITH summary AS (
  SELECT
    track_id as id,
    COUNT() AS cnt,
    MIN(dur) AS min_dur,
    MAX(dur) AS max_dur,
    MAX(depth) AS max_depth
  FROM slice
  GROUP BY track_id
)
SELECT
  s.id,
  s.cnt,
  s.min_dur,
  s.max_dur,
  s.max_depth,
  COALESCE(SUM(
    a.key = 'upid'
    OR a.key = 'utid'
    OR a.key = 'cpu'
  ), 0) = 0 AS is_legacy_global
FROM summary s
JOIN track t USING (id)
LEFT JOIN args a ON t.dimension_arg_set_id = a.arg_set_id
GROUP BY s.id;
