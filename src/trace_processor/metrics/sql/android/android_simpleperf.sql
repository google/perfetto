--
-- Copyright 2021 The Android Open Source Project
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


-- Find all counters from track that satisfies regex 'slc/qurg2_(wr|rd):lvl=0x(0|1|3|7)%'
DROP VIEW IF EXISTS all_qurg2;
CREATE VIEW all_qurg2 AS
SELECT
  ts,
  track_id,
  name,
  value
FROM counters
WHERE name GLOB 'slc/qurg2_??:lvl=0x_*';

-- Find all counters from track that satisfies regex 'slc/qurg2_(wr|rd):lvl=0x(1|3|7)%'
DROP VIEW IF EXISTS non_zero_qurg2;
CREATE VIEW non_zero_qurg2 AS
SELECT
  *
FROM all_qurg2
WHERE name NOT GLOB 'slc/qurg2_??:lvl=0x0*';

DROP VIEW IF EXISTS android_simpleperf_output;
CREATE VIEW android_simpleperf_output AS
SELECT AndroidSimpleperfMetric(
  'urgent_ratio', (SELECT sum(value) FROM non_zero_qurg2) / (SELECT sum(value) FROM all_qurg2)
);
