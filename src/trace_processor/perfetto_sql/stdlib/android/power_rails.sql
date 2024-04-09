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

-- Android power rails counters.
CREATE PERFETTO TABLE android_power_rails_counters (
    -- `counter.id`
    id INT,
    -- Counter timestamp.
    ts INT,
    -- Power rail name. From `counter_track.name`.
    power_rail_name INT,
    -- Power rails counter value in micro watts.
    value DOUBLE
)
AS
SELECT
    c.id,
    c.ts,
    t.name AS power_rail_name,
    c.value
FROM counter c
JOIN counter_track t ON c.track_id = t.id
WHERE name GLOB 'power.*';