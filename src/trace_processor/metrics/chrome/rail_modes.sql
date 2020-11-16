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
-- Priority order for RAIL modes where response has the highest priority and
-- idle has the lowest.
CREATE TABLE IF NOT EXISTS rail_modes (
  mode TEXT UNIQUE,
  ordering INT,
  short_name TEXT
);

-- RAIL_MODE_IDLE is used when no frames are visible in the renderer and so this
-- interprets that as background.
-- RAIL_MODE_LOAD is for the time from a navigation until the first meaningful
-- paint (assuming there are no user interactions).
-- RAIL_MODE_RESPONSE is used when the main thread is dealing with a
-- user-interaction (but not for instance for scrolls which may be handled by
-- the compositor).
-- RAIL_MODE_ANIMATION is used when none of the above apply.
-- The enum in chrome is defined in:
-- https://source.chromium.org/chromium/chromium/src/+/master:third_party/blink/renderer/platform/scheduler/public/rail_mode_observer.h
INSERT
  OR IGNORE INTO rail_modes
VALUES ('RAIL_MODE_IDLE', 0, 'background'),
  ('RAIL_MODE_ANIMATION', 1, "animation"),
  ('RAIL_MODE_LOAD', 2, "load"),
  ('RAIL_MODE_RESPONSE', 3, "response");

-- View containing all Scheduler.RAILMode slices across all Chrome renderer
-- processes.
DROP VIEW IF EXISTS rail_mode_slices;
CREATE VIEW rail_mode_slices AS
SELECT slice.id,
  ts,
  CASE
    WHEN dur == -1 THEN trace_bounds.end_ts - ts
    ELSE dur
  END AS dur,
  track_id,
  EXTRACT_ARG(slice.arg_set_id, "chrome_renderer_scheduler_state.rail_mode") AS rail_mode
FROM trace_bounds,
  slice
WHERE slice.name = "Scheduler.RAILMode";

-- View containing a collapsed view of rail_mode_slices where there is only one
-- RAIL mode active at a given time. The mode is derived using the priority
-- order in rail_modes.
DROP VIEW IF EXISTS overall_rail_mode_slices;
CREATE VIEW overall_rail_mode_slices AS
SELECT s.ts,
  s.end_ts,
  rail_modes.short_name AS rail_mode,
  MAX(rail_modes.ordering)
FROM (
    SELECT ts,
      LEAD(ts, 1, trace_bounds.end_ts) OVER (
        ORDER BY ts
      ) AS end_ts
    FROM (
        SELECT DISTINCT ts
        FROM rail_mode_slices
      ) start_times,
      trace_bounds
  ) s,
  rail_mode_slices r,
  rail_modes
WHERE (
    (
      s.ts >= r.ts AND s.ts < r.ts + r.dur
    )
    OR (
      s.end_ts > r.ts AND s.end_ts <= r.ts + r.dur
    )
  )
  AND r.rail_mode == rail_modes.mode
GROUP BY s.ts;

-- Contains the same data as overall_rail_mode_slices except adjacent slices
-- with the same RAIL mode are combined.
CREATE TABLE IF NOT EXISTS combined_overall_rail_slices AS
SELECT ROW_NUMBER() OVER () AS id,
  ts,
  end_ts - ts AS dur,
  rail_mode
FROM (
    SELECT lag(l.end_ts, 1, FIRST) OVER (
        ORDER BY l.ts
      ) AS ts,
      l.end_ts,
      l.rail_mode
    FROM (
        SELECT ts,
          end_ts,
          rail_mode
        FROM overall_rail_mode_slices s
        WHERE NOT EXISTS (
            SELECT NULL
            FROM overall_rail_mode_slices s2
            WHERE s.rail_mode = s2.rail_mode
              AND s.end_ts = s2.ts
          )
      ) AS l,
      (
        SELECT min(ts) AS FIRST
        FROM overall_rail_mode_slices
      )
  );

-- Now we have the RAIL Mode, use other trace events to create a modified RAIL
-- mode that more accurately reflects what the browser/user are doing.

-- First create slices for when there's no animation as indicated by a large gap
-- between vsync events (since it's easier to find gaps than runs of adjacent
-- vsyncs).

-- Mark any large gaps between vsyncs.
-- The value in "present" doesn't mean anything. It's just there to be a
-- non-NULL value so the later SPAN_JOIN can find the set-difference.
DROP VIEW IF EXISTS not_animating_slices;
CREATE VIEW not_animating_slices AS
WITH const (vsync_padding, large_gap) AS (
  SELECT
    -- Pad 50ms either side of a vsync
    50000000,
    -- A gap of >200ms between the adjacent vsyncs is treated as a gap in
    -- animation.
    200000000
)
SELECT ts + const.vsync_padding AS ts,
  gap_to_next_vsync - const.vsync_padding * 2 AS dur, 1 AS present
FROM const, (SELECT name,
    ts,
    lead(ts) OVER () - ts AS gap_to_next_vsync,
    dur
  FROM slice
  WHERE name = "VSync")
WHERE gap_to_next_vsync > const.large_gap
UNION
-- Insert a slice between start_ts and the first vsync (or the end of the trace
-- if there are none).
SELECT
  ts,
  dur,
  1
FROM (SELECT start_ts AS ts,
    COALESCE(MIN(ts) - start_ts - const.vsync_padding, end_ts - start_ts) AS dur
  FROM trace_bounds, slice, const
  WHERE name = "VSync") WHERE dur > 0
UNION
-- Insert a slice between the last vsync and end_ts
SELECT last_vsync AS ts,
  end_ts - last_vsync AS dur,
  1
FROM (
    SELECT MAX(ts) + const.vsync_padding AS last_vsync
    FROM slice, const
    WHERE name = "VSync"
  ),
  trace_bounds
WHERE last_vsync < end_ts;

-- Since the scheduler defaults to animation when none of the other RAIL modes
-- apply, animation overestimates the amount of time that actual animation is
-- occurring.
-- So instead we try to divide up animation in other buckets based on other
-- trace events.
DROP VIEW IF EXISTS rail_mode_animation_slices;
CREATE VIEW rail_mode_animation_slices AS
SELECT * FROM combined_overall_rail_slices WHERE rail_mode = "animation";

-- Left-join rail mode animation slices with not_animating_slices which is
-- based on the gaps between vsync events.
DROP TABLE IF EXISTS temp_rail_mode_join_animation;
CREATE VIRTUAL TABLE temp_rail_mode_join_animation
USING SPAN_LEFT_JOIN(rail_mode_animation_slices, not_animating_slices);

-- When the RAIL mode is animation, but there is no actual animation (according
-- to vsync data), then record the mode as foreground_idle instead.
DROP VIEW IF EXISTS modified_rail_slices;
CREATE VIEW modified_rail_slices AS
SELECT ROW_NUMBER() OVER () AS id,
  ts,
  dur,
  mode
FROM (
  SELECT
    ts,
    dur,
    IIF(
      present IS NULL,
      "animation",
      "foreground_idle"
    ) AS mode
  FROM temp_rail_mode_join_animation
  UNION
  SELECT ts,
    dur,
    rail_mode AS mode
  FROM combined_overall_rail_slices
  WHERE rail_mode <> "animation");
