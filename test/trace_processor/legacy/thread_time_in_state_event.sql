--
-- Copyright 2020 The Android Open Source Project
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
-- Create so that RUN_METRIC will run without outputting any rows.
SELECT RUN_METRIC('android/android_thread_time_in_state.sql')
    AS suppress_query_output;

SELECT * FROM android_thread_time_in_state_event
ORDER BY ts, upid, track_name;
