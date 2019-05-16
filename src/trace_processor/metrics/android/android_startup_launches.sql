--
-- Copyright 2019 The Android Open Source Project
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

-- Helper to optimize the query for launching events
-- TODO(b/132771327): remove when fixed
CREATE TABLE launching_events_helper AS
SELECT
  arg_set_id,
  STR_SPLIT(STR_SPLIT(args.string_value, "|", 2), ": ", 1) package_name,
  STR_SPLIT(args.string_value, "|", 0) type
FROM args
WHERE string_value LIKE '%|launching: %';

-- TODO: Replace with proper async slices once available
-- The start of the launching event corresponds to the end of the AM handling
-- the startActivity intent, whereas the end corresponds to the first frame drawn.
-- Only successful app launches have a launching event.
CREATE TABLE launching_events AS
SELECT
  ts,
  package_name,
  type
FROM raw
JOIN launching_events_helper USING(arg_set_id)
JOIN thread USING(utid)
JOIN process USING(upid)
WHERE raw.name = 'print' AND process.name = 'system_server';

-- Marks the beginning of the trace and is equivalent to when the statsd launch
-- logging begins.
CREATE VIEW activity_intent_received AS
SELECT ts FROM slices
WHERE name = 'MetricsLogger:launchObserverNotifyIntentStarted';

-- We partition the trace into spans based on posted activity intents.
-- We will refine these progressively in the next steps to only encompass
-- activity starts.
CREATE TABLE activity_intent_recv_spans(id INT, ts BIG INT, dur BIG INT);

INSERT INTO activity_intent_recv_spans
SELECT
  ROW_NUMBER()
    OVER(ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS id,
  ts,
  LEAD(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER(ORDER BY ts) - ts AS dur
FROM activity_intent_received
ORDER BY ts;

-- Filter activity_intent_recv_spans, keeping only the ones that triggered
-- a launch.
CREATE VIEW launches_started AS
SELECT * FROM activity_intent_recv_spans
WHERE 1 = (
  SELECT COUNT(1)
  FROM launching_events
  WHERE TRUE
    AND type = 'S'
    AND activity_intent_recv_spans.ts < ts
    AND ts < activity_intent_recv_spans.ts + activity_intent_recv_spans.dur);

-- All activity launches in the trace, keyed by ID.
CREATE TABLE package_launches_succeeded(
  ts BIG INT,
  ts_end BIG INT,
  dur BIG INT,
  id INT,
  package STRING);

INSERT INTO package_launches_succeeded
SELECT
  started.ts AS ts,
  finished.ts AS ts_end,
  finished.ts - started.ts AS dur,
  started.id AS id,
  finished.package_name AS package
FROM launches_started AS started
JOIN (SELECT * FROM launching_events WHERE type = 'F') AS finished
ON started.ts < finished.ts AND finished.ts <= started.ts + started.dur;

-- Base launches table. A launch is uniquely identified by its id.
CREATE TABLE launches(
  ts BIG INT,
  ts_end BIG INT,
  dur BIG INT,
  id INT,
  package STRING,
  upid BIG INT);

-- We make the (not always correct) simplification that process == package
INSERT INTO launches
SELECT
  ts,
  ts_end,
  dur,
  id,
  package,
  (
    SELECT upid
    FROM process
    WHERE name = pls.package
    AND (start_ts IS NULL OR start_ts < pls.ts_end)
    ORDER BY start_ts DESC
    LIMIT 1) AS upid
FROM package_launches_succeeded AS pls;
