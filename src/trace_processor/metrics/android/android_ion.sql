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

CREATE VIEW ion_timeline AS
SELECT
  ts,
  LEAD(ts, 1, (SELECT end_ts FROM trace_bounds))
    OVER(PARTITION BY counter_id ORDER BY ts) - ts AS dur,
  SUBSTR(name, 9) AS heap_name,
  value
FROM counter_definitions JOIN counter_values USING(counter_id)
WHERE name LIKE 'mem.ion.%' AND ref_type IS NULL;

CREATE VIEW ion_buffers AS
SELECT
  heap_name,
  SUM(value * dur) / SUM(dur) AS avg_size,
  MIN(value) AS min_size,
  MAX(value) AS max_size
FROM ion_timeline
GROUP BY 1;

CREATE VIEW android_ion_output AS
SELECT AndroidIonMetric(
  'buffer', RepeatedField(
    AndroidIonMetric_Buffer(
      'name', heap_name,
      'avg_size_bytes', avg_size,
      'min_size_bytes', min_size,
      'max_size_bytes', max_size
    )
  ))
FROM ion_buffers;
