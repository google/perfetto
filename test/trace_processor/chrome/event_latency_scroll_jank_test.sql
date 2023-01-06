--
-- Copyright 2022 The Android Open Source Project
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

SELECT RUN_METRIC('chrome/event_latency_scroll_jank.sql');

SELECT
  jank,
  next_jank,
  prev_jank,
  gesture_begin_ts,
  gesture_end_ts,
  ts,
  dur,
  dur_before_show_on_screen,
  event_type,
  next_ts,
  next_dur,
  prev_ts,
  prev_dur
FROM scroll_event_latency_jank
ORDER BY jank DESC
LIMIT 10;
