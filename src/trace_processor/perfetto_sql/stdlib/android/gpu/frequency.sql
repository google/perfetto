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

INCLUDE PERFETTO MODULE counters.intervals;

-- GPU frequency counter per GPU.
CREATE PERFETTO TABLE android_gpu_frequency(
  -- Timestamp
  ts INT,
  -- Duration
  dur INT,
  -- GPU id. Joinable with `gpu_counter_track.gpu_id`.
  gpu_id INT,
  -- GPU frequency
  gpu_freq INT
) AS
SELECT
  ts,
  dur,
  gpu_id,
  cast_int!(value) AS gpu_freq
FROM counter_leading_intervals!((
    SELECT c.*
    FROM counter c
    JOIN gpu_counter_track t
    ON t.id = c.track_id AND t.name = 'gpufreq'
    WHERE gpu_id IS NOT NULL
))
JOIN gpu_counter_track t ON t.id = track_id;
