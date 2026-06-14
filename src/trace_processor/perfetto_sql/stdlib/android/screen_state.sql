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

-- NOTE (psqlnext): the `counters.intervals` and `intervals.fill_gaps` modules are
-- DELETED — `counter_leading_intervals!` is `INTERVALS FROM EVENTS` (+`MERGE
-- CONSECUTIVE BY value`), and `_intervals_fill_gaps!` is `INTERVAL FILL … WITHIN`.

-- Table of the screen state - on, off or doze (always on display).
CREATE PERFETTO PIPELINE android_screen_state(
  -- ID.
  id ID,
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration.
  dur DURATION,
  -- Simplified screen state: 'unknown', 'off', 'doze' (AoD) or 'on'
  simple_screen_state STRING,
  -- Full screen state, adding VR and suspended-while-displaying states.
  short_screen_state STRING,
  -- Human-readable string.
  screen_state STRING
)
MATERIALIZED AS
SUBPIPELINE screen_state_events AS (
  FROM counter
  |> JOIN counter_track ON counter_track.id = counter.track_id
  |> WHERE counter_track.name = 'ScreenState'
  |> SELECT counter.id, counter.ts, 0 AS track_id, counter.value
)
-- Counter samples become intervals of constant state; equal-valued runs coalesce.
INTERVALS FROM EVENTS screen_state_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
|> WHERE dur > 0
|> SELECT
     ts,
     dur,
     -- Should be kept in sync with the enums in Display.java
     CASE value
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
     CASE value
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
     CASE value
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
-- Cover the whole trace: spans with no slice become filler rows with null
-- payload, defaulted below.
|> INTERVAL FILL WITHIN trace_bounds
|> SELECT
     ROW_NUMBER() OVER () AS id,
     ts,
     dur,
     COALESCE(simple_screen_state, 'unknown') AS simple_screen_state,
     COALESCE(short_screen_state, 'unknown') AS short_screen_state,
     COALESCE(screen_state, 'Unknown') AS screen_state;
