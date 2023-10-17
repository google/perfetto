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
CREATE VIEW freq_view AS
SELECT
  ts,
  lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
  cpu,
  name AS freq_name,
  value AS freq_value
FROM counter
JOIN cpu_counter_track
  ON counter.track_id = cpu_counter_track.id
WHERE name = 'cpufreq';

CREATE VIEW idle_view
AS SELECT
  ts,
  lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
  cpu,
  name AS idle_name,
  value AS idle_value
FROM counter
JOIN cpu_counter_track
  ON counter.track_id = cpu_counter_track.id
WHERE name = 'cpuidle';

CREATE VIRTUAL TABLE freq_idle
USING span_join(freq_view PARTITIONED cpu, idle_view PARTITIONED cpu);

CREATE VIRTUAL TABLE window_freq_idle USING window;

CREATE VIRTUAL TABLE span_freq_idle
USING span_join(freq_idle PARTITIONED cpu, window_freq_idle);

UPDATE window_freq_idle
SET
  window_start = (SELECT min(ts) FROM sched),
  window_dur = (SELECT max(ts) - min(ts) FROM sched),
  quantum = 1000000
WHERE rowid = 0;

CREATE VIEW counter_view
AS SELECT
  ts,
  dur,
  quantum_ts,
  cpu,
  CASE idle_value
    WHEN 4294967295 THEN "freq"
    ELSE "idle"
  END AS name,
  CASE idle_value
    WHEN 4294967295 THEN freq_value
    ELSE idle_value
  END AS value
FROM span_freq_idle;

SELECT cpu, name, value, sum(dur) FROM counter_view GROUP BY cpu, name, value;
