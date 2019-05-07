--
-- Copyright 2019 The Android Open Source Project
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

CREATE VIEW {{table_name}}_span AS
SELECT
  ts,
  LEAD(ts, 1, ts) OVER(PARTITION BY counter_id ORDER BY ts) - ts AS dur,
  ref AS upid,
  value
FROM counters
WHERE name IN {{counter_names}} AND ref IS NOT NULL AND ref_type = 'upid';

CREATE VIEW {{table_name}} AS
SELECT
  process.name,
  MIN(span.value),
  MAX(span.value),
  SUM(span.value * span.dur) / SUM(span.dur)
FROM {{table_name}}_span as span JOIN process USING(upid)
WHERE NOT (process.name IS NULL OR process.name = '')
GROUP BY 1
HAVING SUM(span.dur) > 0
ORDER BY 1;
