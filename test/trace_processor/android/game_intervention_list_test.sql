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
--

SELECT
  package_name,
  uid,
  current_mode,
  perf_mode_supported,
  perf_mode_downscale,
  perf_mode_use_angle,
  perf_mode_fps,
  battery_mode_supported,
  battery_mode_downscale,
  battery_mode_use_angle,
  battery_mode_fps
FROM android_game_intervention_list
ORDER BY package_name
