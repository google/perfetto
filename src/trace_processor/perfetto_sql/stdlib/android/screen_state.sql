-- Copyright 2025 The Android Open Source Project
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

INCLUDE PERFETTO MODULE counters.intervals;

INCLUDE PERFETTO MODULE intervals.fill_gaps;

-- Table of the screen state - on, off or doze (always on display).
CREATE PERFETTO TABLE android_screen_state(
  -- ID.
  id ID,
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration.
  dur DURATION,
  -- Machine whose screen state is described.
  machine_id JOINID(machine.id),
  -- Simplified screen state: 'unknown', 'off', 'doze' (AoD) or 'on'
  simple_screen_state STRING,
  -- Full screen state, adding VR and suspended-while-displaying states.
  short_screen_state STRING,
  -- Human-readable string.
  screen_state STRING
)
AS
WITH
  screen_state_span AS (
    SELECT *
    FROM counter_leading_intervals!((
        SELECT counter.id, ts, counter.track_id, value
        FROM counter
        JOIN counter_track ON counter_track.id = counter.track_id
        WHERE
          name = 'ScreenState'
      ))
  ),
  mapped_names AS (
    SELECT
      screen_state_span.id,
      screen_state_span.ts,
      screen_state_span.dur,
      counter_track.machine_id,
      -- Should be kept in sync with the enums in Display.java
      CASE screen_state_span.value
        -- Display.STATE_OFF
        WHEN 1 THEN 'off'
        -- Display.STATE_ON
        WHEN 2 THEN 'on'
        -- Display.STATE_DOZE
        WHEN 3 THEN 'doze'
        -- Display.STATE_DOZE_SUSPEND
        WHEN 4 THEN 'doze'
        -- Display.STATE_VR
        WHEN 5 THEN 'on'
        -- Display.STATE_ON_SUSPEND
        WHEN 6 THEN 'on'
      END AS simple_screen_state,
      CASE screen_state_span.value
        -- Display.STATE_OFF
        WHEN 1 THEN 'off'
        -- Display.STATE_ON
        WHEN 2 THEN 'on'
        -- Display.STATE_DOZE
        WHEN 3 THEN 'doze'
        -- Display.STATE_DOZE_SUSPEND
        WHEN 4 THEN 'doze-suspend'
        -- Display.STATE_VR
        WHEN 5 THEN 'on-vr'
        -- Display.STATE_ON_SUSPEND
        WHEN 6 THEN 'on-suspend'
      END AS short_screen_state,
      CASE screen_state_span.value
        -- Display.STATE_OFF
        WHEN 1 THEN 'Screen off'
        -- Display.STATE_ON
        WHEN 2 THEN 'Screen on'
        -- Display.STATE_DOZE
        WHEN 3 THEN 'Always-on display (doze)'
        -- Display.STATE_DOZE_SUSPEND
        WHEN 4 THEN 'Always-on display (doze-suspend)'
        -- Display.STATE_VR
        WHEN 5 THEN 'Screen on (VR)'
        -- Display.STATE_ON_SUSPEND
        WHEN 6 THEN 'Screen on (suspend)'
      END AS screen_state
    FROM screen_state_span
    JOIN counter_track
      ON counter_track.id = screen_state_span.track_id
    WHERE
      dur > 0
  )
SELECT
  ROW_NUMBER() OVER () AS id,
  ts,
  dur,
  machine_id,
  COALESCE(simple_screen_state, 'unknown') AS simple_screen_state,
  COALESCE(short_screen_state, 'unknown') AS short_screen_state,
  COALESCE(screen_state, 'Unknown') AS screen_state
FROM _intervals_fill_gaps!((machine_id), (simple_screen_state,
  short_screen_state,
  screen_state), mapped_names);
