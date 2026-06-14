--
-- Copyright 2026 The Android Open Source Project
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

-- Extract GPU task slices from the work period track.
CREATE PERFETTO PIPELINE _gpu_tasks MATERIALIZED AS
FROM slice AS s
|> JOIN android_gpu_work_period_track AS t ON s.track_id = t.id
|> WHERE s.dur > 0
|> SELECT s.ts, s.dur, t.uid, t.gpu_id;

-- Calculate the number of active GPU tasks at any point in time.
CREATE PERFETTO PIPELINE _gpu_active_task_count MATERIALIZED AS
FROM _gpu_tasks
|> INTERVAL FLATTEN AGGREGATE COUNT(*) AS active_tasks
|> INTERVAL FILL WITHIN trace_bounds
|> EXTEND active_tasks = coalesce(active_tasks, 0)
|> WHERE dur > 0
|> SELECT ts, dur, active_tasks;
