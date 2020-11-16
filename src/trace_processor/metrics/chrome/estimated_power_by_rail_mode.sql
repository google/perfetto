--
-- Copyright 2020 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

SELECT RUN_METRIC('chrome/rail_modes.sql');

-- Creates a view called power_by_rail_mode, containing the estimated CPU power
-- usage for chrome broken down by RAIL Mode.
SELECT RUN_METRIC(
    'chrome/estimated_power_by_category.sql',
    'input', 'combined_overall_rail_slices',
    'output', 'power_by_rail_mode',
    'category', 'rail_mode'
  );
