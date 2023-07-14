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

-- Marks the beginning of the trace and is equivalent to when the statsd startup
-- logging begins.
CREATE VIEW internal_activity_intent_received AS
SELECT ts FROM slice
WHERE name = 'MetricsLogger:launchObserverNotifyIntentStarted';

-- We partition the trace into spans based on posted activity intents.
-- We will refine these progressively in the next steps to only encompass
-- activity starts.
CREATE PERFETTO TABLE internal_activity_intent_recv_spans AS
SELECT
  ROW_NUMBER()
  OVER(ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS startup_id,
  ts,
  LEAD(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER(ORDER BY ts) - ts AS dur
FROM internal_activity_intent_received
ORDER BY ts;

-- Filter activity_intent_recv_spans, keeping only the ones that triggered
-- a startup.
CREATE VIEW internal_startup_partitions AS
SELECT * FROM internal_activity_intent_recv_spans AS spans
WHERE 1 = (
  SELECT COUNT(1)
  FROM internal_startup_events
  WHERE internal_startup_events.ts BETWEEN spans.ts AND spans.ts + spans.dur);

-- Successful activity startup. The end of the 'launching' event is not related
-- to whether it actually succeeded or not.
CREATE VIEW internal_activity_intent_startup_successful AS
SELECT ts FROM slice
WHERE name = 'MetricsLogger:launchObserverNotifyActivityLaunchFinished';

-- Use the starting event package name. The finish event package name
-- is not reliable in the case of failed startups.
INSERT INTO internal_all_startups
SELECT
  "minsdk29",
  lpart.startup_id,
  lpart.ts,
  le.ts_end,
  le.ts_end - lpart.ts AS dur,
  package_name AS package,
  NULL AS startup_type
FROM internal_startup_partitions AS lpart
JOIN internal_startup_events le ON
  (le.ts BETWEEN lpart.ts AND lpart.ts + lpart.dur)
  AND (le.ts_end BETWEEN lpart.ts AND lpart.ts + lpart.dur)
WHERE (
  SELECT COUNT(1)
  FROM internal_activity_intent_startup_successful AS successful
  WHERE successful.ts BETWEEN lpart.ts AND lpart.ts + lpart.dur
) > 0;
