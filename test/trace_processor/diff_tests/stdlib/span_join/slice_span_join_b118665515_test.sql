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
CREATE VIRTUAL TABLE window_8 USING __intrinsic_window(81473010031230, 19684693341, 10000000);

CREATE VIRTUAL TABLE span_8 USING span_join(sched PARTITIONED cpu, window_8);

SELECT quantum_ts AS bucket, sum(dur) / cast_double!(10000000) AS utilization FROM span_8 WHERE cpu = 7 AND NOT utid IN (SELECT utid FROM thread WHERE is_idle) GROUP BY quantum_ts;
