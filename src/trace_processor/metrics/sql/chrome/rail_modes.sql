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

SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT RUN_METRIC('chrome/chrome_event_metadata.sql');

-- Priority order for RAIL modes where response has the highest priority and
-- idle has the lowest.
DROP TABLE IF EXISTS rail_modes;
CREATE TABLE rail_modes (
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
INSERT INTO rail_modes
VALUES ('RAIL_MODE_IDLE', 0, 'background'),
('RAIL_MODE_ANIMATION', 1, "animation"),
('RAIL_MODE_LOAD', 2, "load"),
('RAIL_MODE_RESPONSE', 3, "response");


-- Find the max ts + dur for every process
DROP TABLE IF EXISTS max_ts_per_process;
CREATE PERFETTO TABLE max_ts_per_process AS
-- MAX(dur, 0) means unclosed slices just contribute their start time.
SELECT upid,
  MAX(ts + MAX(dur, 0)) AS ts
FROM (
    SELECT upid,
      ts,
      dur
    FROM process_track t
    JOIN slice s
    WHERE s.track_id = t.id
    UNION ALL
    SELECT upid,
      ts,
      dur
    FROM thread_track t
    JOIN thread
    JOIN slice
    WHERE slice.track_id = t.id
      AND thread.utid = t.utid
  )
GROUP BY upid;

-- View containing all Scheduler.RAILMode slices across all Chrome renderer
-- processes.
DROP VIEW IF EXISTS original_rail_mode_slices;
CREATE PERFETTO VIEW original_rail_mode_slices AS
SELECT slice.id,
  slice.ts,
  CASE
    -- Add 1 to the duration to ensure you cannot get a zero-sized RAIL mode
    -- slice, which can throw off the later queries.
    WHEN dur = -1 THEN max_ts_per_process.ts - slice.ts + 1
    ELSE dur
  END AS dur,
  track_id,
  EXTRACT_ARG(
    slice.arg_set_id,
    "chrome_renderer_scheduler_state.rail_mode"
  ) AS rail_mode
FROM max_ts_per_process,
  slice,
  process_track
WHERE slice.name = "Scheduler.RAILMode"
  AND slice.track_id = process_track.id
  AND process_track.upid = max_ts_per_process.upid;

-- Detect if the trace has an unrealistic length (10 minutes) that probably
-- means some trace events have faulty timestamps and which could throw off any
-- metrics that use the trace.
DROP VIEW IF EXISTS trace_has_realistic_length;
CREATE PERFETTO VIEW trace_has_realistic_length AS
SELECT trace_dur() < 1e9 * 60 * 10 AS value;

-- RAIL_MODE_LOAD seems to get stuck which makes it not very useful so remap it
-- to RAIL_MODE_ANIMATION so it doesn't dominate the overall RAIL mode.
DROP VIEW IF EXISTS rail_mode_slices;
CREATE PERFETTO VIEW rail_mode_slices AS
SELECT ts, dur, track_id,
  CASE
    WHEN rail_mode = "RAIL_MODE_LOAD" THEN "RAIL_MODE_ANIMATION"
    ELSE rail_mode
  END AS rail_mode
FROM original_rail_mode_slices;

-- View containing a collapsed view of rail_mode_slices where there is only one
-- RAIL mode active at a given time. The mode is derived using the priority
-- order in rail_modes.
DROP VIEW IF EXISTS overall_rail_mode_slices;
CREATE PERFETTO VIEW overall_rail_mode_slices AS
SELECT s.ts,
  s.end_ts,
  rail_modes.short_name AS rail_mode,
  MAX(rail_modes.ordering)
FROM (
    SELECT ts,
      LEAD(ts, 1, (SELECT MAX(ts + dur) FROM rail_mode_slices)) OVER (
        ORDER BY ts
      ) AS end_ts
    FROM (
        SELECT DISTINCT ts
        FROM rail_mode_slices
      ) start_times
  ) s,
  rail_mode_slices r,
  rail_modes,
  trace_has_realistic_length
WHERE (
    (
      s.ts >= r.ts AND s.ts < r.ts + r.dur
    )
    OR (
      s.end_ts > r.ts AND s.end_ts <= r.ts + r.dur
    )
  )
  AND r.rail_mode = rail_modes.mode
  AND trace_has_realistic_length.value
GROUP BY s.ts;

-- Contains the same data as overall_rail_mode_slices except adjacent slices
-- with the same RAIL mode are combined.
DROP TABLE IF EXISTS combined_overall_rail_slices;
CREATE PERFETTO TABLE combined_overall_rail_slices AS
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
-- The value in "not_animating" is always 1. It's just there to be a non-NULL
-- value so the later SPAN_JOIN can find the set-difference.
DROP VIEW IF EXISTS not_animating_slices;
CREATE PERFETTO VIEW not_animating_slices AS
WITH const (vsync_padding, large_gap) AS (
  SELECT
    -- Pad 50ms either side of a vsync
    50000000,
    -- A gap of >200ms between the adjacent vsyncs is treated as a gap in
    -- animation.
    200000000
)
SELECT ts + const.vsync_padding AS ts,
  gap_to_next_vsync - const.vsync_padding * 2 AS dur, 1 AS not_animating
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
  COALESCE(
    (
    SELECT MIN(ts)
    FROM slice
    WHERE name = "VSync"
) - start_ts - const.vsync_padding,
  end_ts - start_ts
  ) AS dur
  FROM trace_bounds, const)
WHERE dur > 0
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

-- There are two types of InputLatency:: events:
-- 1) Simple ones that begin at ts and end at ts+dur
-- 2) Paired ones that begin with a "begin" slice and end at an "end" slice.
--
-- Paired events are even trickier because we can't guarantee that the "begin"
-- slice will even be in the trace and because it's possible for multiple begin
-- slices to appear without an intervening end slice.

-- Table of begin and end events along with the increment/decrement to be
-- applied to the appropriate counter (one for each type of paired event). Final
-- column dur_multiplier is used to find the timestamp to mark the event at in
-- the equation event_ts = ts + dur * dur_multiplier. End events have
-- dur_multiplier of 1, which makes their ts the end of the slice rather than
-- the start.
DROP TABLE IF EXISTS input_latency_begin_end_names;
CREATE TABLE input_latency_begin_end_names
(
  full_name TEXT UNIQUE,
  prefix TEXT,
  scroll_increment INT,
  pinch_increment INT,
  touch_increment INT,
  fling_increment INT,
  pointer_increment INT,
  dur_multiplier INT
);

INSERT
OR IGNORE INTO input_latency_begin_end_names
VALUES
("InputLatency::GestureScrollBegin",
  "InputLatency::GestureScroll", 1, 0, 0, 0, 0, 0),
("InputLatency::GestureScrollEnd",
  "InputLatency::GestureScroll", -1, 0, 0, 0, 0, 1),
("InputLatency::GesturePinchBegin",
  "InputLatency::GesturePinch", 0, 1, 0, 0, 0, 0),
("InputLatency::GesturePinchEnd",
  "InputLatency::GesturePinch", 0, -1, 0, 0, 0, 1),
("InputLatency::TouchStart",
  "InputLatency::Touch", 0, 0, 1, 0, 0, 0),
("InputLatency::TouchEnd",
  "InputLatency::Touch", 0, 0, -1, 0, 0, 1),
("InputLatency::GestureFlingStart",
  "InputLatency::GestureFling", 0, 0, 0, 1, 0, 0),
("InputLatency::GestureFlingCancel",
  "InputLatency::GestureFling", 0, 0, 0, -1, 0, 1),
("InputLatency::PointerDown",
  "InputLatency::Pointer", 0, 0, 0, 0, 1, 0),
("InputLatency::PointerUp",
  "InputLatency::Pointer", 0, 0, 0, 0, -1, 1),
("InputLatency::PointerCancel",
  "InputLatency::Pointer", 0, 0, 0, 0, -1, 1);

-- Find all the slices that have split "begin" and "end" slices and maintain a
-- running total for each type, where >0 means that type of input event is
-- ongoing.
DROP VIEW IF EXISTS input_begin_end_slices;
CREATE PERFETTO VIEW input_begin_end_slices AS
SELECT prefix,
  -- Mark the change at the start of "start" slices and the end of "end" slices.
  ts + dur * dur_multiplier AS ts,
  scroll_increment,
  pinch_increment,
  touch_increment,
  fling_increment,
  pointer_increment
FROM slice
JOIN input_latency_begin_end_names ON name = full_name
ORDER BY ts;

-- Combine all the paired input events to get an indication of when any paired
-- input event is ongoing.
DROP VIEW IF EXISTS unified_input_pair_increments;
CREATE PERFETTO VIEW unified_input_pair_increments AS
SELECT ts,
  scroll_increment
  + pinch_increment
  + touch_increment
  + fling_increment
  + pointer_increment AS increment
FROM input_begin_end_slices;

-- It's possible there's an end slice without a start slice (as it occurred
-- before the trace started) which would result in (starts - ends) going
-- negative at some point. So find an offset that shifts up all counts so the
-- lowest values becomes zero. It's possible this could still do the wrong thing
-- if there were start AND end slices that are outside the trace bounds, in
-- which case it should count as covering the entire trace, but it's impossible
-- to compensate for that without augmenting the trace events themselves.
DROP VIEW IF EXISTS initial_paired_increment;
CREATE PERFETTO VIEW initial_paired_increment AS
SELECT ts,
  MIN(0, MIN(scroll_total))
  + MIN(0, MIN(pinch_total))
  + MIN(0, MIN(touch_total))
  + MIN(0, MIN(fling_total))
  + MIN(0, MIN(pointer_total)) AS offset
FROM (
    SELECT ts,
      SUM(scroll_increment) OVER(ROWS UNBOUNDED PRECEDING) AS scroll_total,
      SUM(pinch_increment) OVER(ROWS UNBOUNDED PRECEDING) AS pinch_total,
      SUM(touch_increment) OVER(ROWS UNBOUNDED PRECEDING) AS touch_total,
      SUM(fling_increment) OVER(ROWS UNBOUNDED PRECEDING) AS fling_total,
      SUM(pointer_increment) OVER(ROWS UNBOUNDED PRECEDING) AS pointer_total
    FROM input_begin_end_slices
  );

-- Now find all the simple input slices that fully enclose the input they're
-- marking (i.e. not the start or end of a pair).
DROP VIEW IF EXISTS simple_input_slices;
CREATE PERFETTO VIEW simple_input_slices AS
SELECT id,
  name,
  ts,
  dur
FROM slice s
WHERE name GLOB "InputLatency::*"
  AND NOT EXISTS (
    SELECT 1
    FROM slice
    JOIN input_latency_begin_end_names
    WHERE s.name = full_name
  );

-- Turn the simple input slices into +1s and -1s at the start and end of each
-- slice.
DROP VIEW IF EXISTS simple_input_increments;
CREATE PERFETTO VIEW simple_input_increments AS
SELECT ts,
  1 AS increment
FROM simple_input_slices
UNION ALL
SELECT ts + dur,
  -1
FROM simple_input_slices
ORDER BY ts;

-- Combine simple and paired inputs into one, summing all the increments at a
-- given ts.
DROP VIEW IF EXISTS all_input_increments;
CREATE PERFETTO VIEW all_input_increments AS
SELECT ts,
  SUM(increment) AS increment
FROM (
    SELECT *
    FROM simple_input_increments
    UNION ALL
    SELECT *
    FROM unified_input_pair_increments
    ORDER BY ts
  )
GROUP BY ts;

-- Now calculate the cumulative sum of the increments as each ts, giving the
-- total number of outstanding input events at a given time.
DROP VIEW IF EXISTS all_input_totals;
CREATE PERFETTO VIEW all_input_totals AS
SELECT ts,
  SUM(increment) OVER(ROWS UNBOUNDED PRECEDING) > 0 AS input_total
FROM all_input_increments;

-- Now find the transitions from and to 0 and use that to create slices where
-- input events were occurring. The input_active column always contains 1, but
-- is there so that the SPAN_JOIN_LEFT can put NULL in it for RAIL Mode slices
-- that do not have corresponding input events.
DROP VIEW IF EXISTS all_input_slices;
CREATE PERFETTO VIEW all_input_slices AS
SELECT cast(ts as int) as ts,
  dur,
  input_active
FROM (
    SELECT ts,
      lead(ts, 1, end_ts) OVER() - ts AS dur,
      input_active
    FROM trace_bounds,
      (
        SELECT ts,
          input_total > 0 AS input_active
        FROM (
            SELECT ts,
              input_total,
              lag(input_total) OVER() AS prev_input_total
            FROM all_input_totals
          )
        WHERE (input_total > 0 != prev_input_total > 0)
          OR prev_input_total IS NULL
      )
  )
WHERE input_active > 0;

-- Since the scheduler defaults to animation when none of the other RAIL modes
-- apply, animation overestimates the amount of time that actual animation is
-- occurring.
-- So instead we try to divide up animation in other buckets based on other
-- trace events.
DROP VIEW IF EXISTS rail_mode_animation_slices;
CREATE PERFETTO VIEW rail_mode_animation_slices AS
SELECT * FROM combined_overall_rail_slices WHERE rail_mode = "animation";

-- Left-join rail mode animation slices with all_input_slices to find all
-- "animation" slices that should actually be labelled "response".
DROP TABLE IF EXISTS rail_mode_join_inputs;
CREATE VIRTUAL TABLE rail_mode_join_inputs
USING SPAN_LEFT_JOIN(rail_mode_animation_slices, all_input_slices);

-- Left-join rail mode animation slices with not_animating_slices which is
-- based on the gaps between vsync events.
DROP TABLE IF EXISTS rail_mode_join_inputs_join_animation;
CREATE VIRTUAL TABLE rail_mode_join_inputs_join_animation
USING SPAN_LEFT_JOIN(rail_mode_join_inputs, not_animating_slices);

DROP VIEW IF EXISTS has_modified_rail_slices;
CREATE PERFETTO VIEW has_modified_rail_slices AS
SELECT (
    SELECT value
    FROM chrome_event_metadata
    WHERE name = "os-name"
  ) = "Android" AS value;

-- Mapping to allow CamelCased names to be produced from the modified rail
-- modes.
DROP TABLE IF EXISTS modified_rail_mode_prettier;
CREATE TABLE modified_rail_mode_prettier (
  orig_name TEXT UNIQUE,
  pretty_name TEXT
);
INSERT INTO modified_rail_mode_prettier
VALUES ("background", "Background"),
("foreground_idle", "ForegroundIdle"),
("animation", "Animation"),
("load", "Load"),
("response", "Response");

-- When the RAIL mode is animation, use input/vsync data to conditionally change
-- the mode to response or foreground_idle.
DROP VIEW IF EXISTS unmerged_modified_rail_slices;
CREATE PERFETTO VIEW unmerged_modified_rail_slices AS
SELECT ROW_NUMBER() OVER () AS id,
  ts,
  dur,
  mode
FROM (
    SELECT ts,
      dur,
      CASE
        WHEN input_active IS NOT NULL THEN "response"
        WHEN not_animating IS NULL THEN "animation"
        ELSE "foreground_idle"
      END AS mode
    FROM rail_mode_join_inputs_join_animation
    UNION
    SELECT ts,
      dur,
      rail_mode AS mode
    FROM combined_overall_rail_slices
    WHERE rail_mode != "animation"
  )
-- Since VSync events are only emitted on Android (and the concept of a
-- unified RAIL mode only makes sense if there's just a single Chrome window),
-- don't output anything on other platforms. This will result in all the power
-- and cpu time tables being empty rather than containing bogus results.
WHERE (
    SELECT value
    FROM has_modified_rail_slices
  );

-- The previous query creating unmerged_modified_rail_slices, can create
-- adjacent slices with the same mode. This merges them together as well as
-- adding a unique id to each slice. Rather than directly merging slices
-- together, this instead looks for all the transitions and uses this to
-- reconstruct the slices that should occur between them.
DROP TABLE IF EXISTS modified_rail_slices;
CREATE PERFETTO TABLE modified_rail_slices AS
WITH const (end_ts) AS (SELECT ts + dur
  FROM unmerged_modified_rail_slices
  ORDER BY ts DESC
  LIMIT 1
)
SELECT ROW_NUMBER() OVER () AS id, lag(next_ts) OVER() AS ts,
  ts + dur - lag(next_ts) OVER() AS dur,
  mode AS mode
FROM (
    -- For each row in the original table, create a new row with the information
    -- from the following row, since you can't use lag/lead in WHERE clause.
    --
    -- Transition row at the beginning. "mode" is invalid, so a transition will
    -- always be recorded.
    SELECT *
    FROM (SELECT
          0 AS ts,
          ts AS dur,
          "" AS mode,
          ts AS next_ts,
          dur AS next_dur,
          mode AS next_mode
        FROM unmerged_modified_rail_slices
        LIMIT 1
      )
    UNION ALL
    SELECT ts,
      dur,
      mode,
      lead(ts, 1, end_ts) OVER() AS next_ts,
      lead(dur) OVER() AS next_dur,
      lead(mode) OVER() AS next_mode
    FROM unmerged_modified_rail_slices, const
    UNION ALL
    -- Transition row at the end. "next_mode" is invalid, so a transition will
    -- always be recorded.
    SELECT *
    FROM (SELECT
          ts + dur AS ts,
          0 AS dur,
          mode,
          ts + dur AS next_ts,
          0,
          "" AS next_mode
        FROM unmerged_modified_rail_slices
        ORDER BY ts DESC
        LIMIT 1
      )
  )
WHERE mode != next_mode
-- Retrieve all but the first row.
LIMIT -1 OFFSET 1;
