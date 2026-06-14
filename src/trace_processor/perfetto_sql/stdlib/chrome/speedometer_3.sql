-- Copyright 2024 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

-- List Speedometer 3 measures. Used to find relevant slices.
CREATE PERFETTO PIPELINE _chrome_speedometer_3_measure_name (
  -- Expected slice name
  name STRING,
  -- Suite name
  suite_name STRING,
  -- Test name
  test_name STRING,
  -- Measure type
  measure_type STRING
) AS
SUBPIPELINE data AS (
  FROM (VALUES
    ('TodoMVC-JavaScript-ES5', 'Adding100Items'),
    ('TodoMVC-JavaScript-ES5', 'CompletingAllItems'),
    ('TodoMVC-JavaScript-ES5', 'DeletingAllItems'),
    ('TodoMVC-JavaScript-ES6-Webpack-Complex-DOM', 'Adding100Items'),
    ('TodoMVC-JavaScript-ES6-Webpack-Complex-DOM', 'CompletingAllItems'),
    ('TodoMVC-JavaScript-ES6-Webpack-Complex-DOM', 'DeletingAllItems'),
    ('TodoMVC-WebComponents', 'Adding100Items'),
    ('TodoMVC-WebComponents', 'CompletingAllItems'),
    ('TodoMVC-WebComponents', 'DeletingAllItems'),
    ('TodoMVC-React-Complex-DOM', 'Adding100Items'),
    ('TodoMVC-React-Complex-DOM', 'CompletingAllItems'),
    ('TodoMVC-React-Complex-DOM', 'DeletingAllItems'),
    ('TodoMVC-React-Redux', 'Adding100Items'),
    ('TodoMVC-React-Redux', 'CompletingAllItems'),
    ('TodoMVC-React-Redux', 'DeletingAllItems'),
    ('TodoMVC-Backbone', 'Adding100Items'),
    ('TodoMVC-Backbone', 'CompletingAllItems'),
    ('TodoMVC-Backbone', 'DeletingAllItems'),
    ('TodoMVC-Angular-Complex-DOM', 'Adding100Items'),
    ('TodoMVC-Angular-Complex-DOM', 'CompletingAllItems'),
    ('TodoMVC-Angular-Complex-DOM', 'DeletingAllItems'),
    ('TodoMVC-Vue', 'Adding100Items'),
    ('TodoMVC-Vue', 'CompletingAllItems'),
    ('TodoMVC-Vue', 'DeletingAllItems'),
    ('TodoMVC-jQuery', 'Adding100Items'),
    ('TodoMVC-jQuery', 'CompletingAllItems'),
    ('TodoMVC-jQuery', 'DeletingAllItems'),
    ('TodoMVC-Preact-Complex-DOM', 'Adding100Items'),
    ('TodoMVC-Preact-Complex-DOM', 'CompletingAllItems'),
    ('TodoMVC-Preact-Complex-DOM', 'DeletingAllItems'),
    ('TodoMVC-Svelte-Complex-DOM', 'Adding100Items'),
    ('TodoMVC-Svelte-Complex-DOM', 'CompletingAllItems'),
    ('TodoMVC-Svelte-Complex-DOM', 'DeletingAllItems'),
    ('TodoMVC-Lit-Complex-DOM', 'Adding100Items'),
    ('TodoMVC-Lit-Complex-DOM', 'CompletingAllItems'),
    ('TodoMVC-Lit-Complex-DOM', 'DeletingAllItems'),
    ('NewsSite-Next', 'NavigateToUS'),
    ('NewsSite-Next', 'NavigateToWorld'),
    ('NewsSite-Next', 'NavigateToPolitics'),
    ('NewsSite-Nuxt', 'NavigateToUS'),
    ('NewsSite-Nuxt', 'NavigateToWorld'),
    ('NewsSite-Nuxt', 'NavigateToPolitics'),
    ('Editor-CodeMirror', 'Long'),
    ('Editor-CodeMirror', 'Highlight'),
    ('Editor-TipTap', 'Long'),
    ('Editor-TipTap', 'Highlight'),
    ('Charts-observable-plot', 'Stacked by 6'),
    ('Charts-observable-plot', 'Stacked by 20'),
    ('Charts-observable-plot', 'Dotted'),
    ('Charts-chartjs', 'Draw scatter'),
    ('Charts-chartjs', 'Show tooltip'),
    ('Charts-chartjs', 'Draw opaque scatter'),
    ('React-Stockcharts-SVG', 'Render'),
    ('React-Stockcharts-SVG', 'PanTheChart'),
    ('React-Stockcharts-SVG', 'ZoomTheChart'),
    ('Perf-Dashboard', 'Render'),
    ('Perf-Dashboard', 'SelectingPoints'),
    ('Perf-Dashboard', 'SelectingRange')) AS _values(suite_name, test_name)
)
SUBPIPELINE measure_type AS (
  FROM (VALUES ('sync'), ('async')) AS _values(measure_type)
)
FROM data
|> JOIN measure_type ON 1
|> SELECT
     data.suite_name || '.' || data.test_name || '-' || measure_type.measure_type AS name,
     data.suite_name,
     data.test_name,
     measure_type.measure_type;

CREATE PERFETTO PIPELINE _chrome_speedometer_3_iteration_slice AS
FROM slice
|> WHERE category = 'blink.user_timing' AND name GLOB 'iteration-*'
|> EXTEND substr(name, 1 + length('iteration-')) AS iteration_str
|> EXTEND cast_int!(iteration_str) AS iteration
|> WHERE iteration_str = iteration;

-- Augmented slices for Speedometer measurements.
-- These are the intervals of time Speedometer uses to compute the final score.
-- There are two intervals that are measured for every test: sync and async.
CREATE PERFETTO PIPELINE chrome_speedometer_3_measure (
  -- Start timestamp of the measure slice
  ts TIMESTAMP,
  -- Duration of the measure slice
  dur DURATION,
  -- Full measure name
  name STRING,
  -- Speedometer iteration the slice belongs to.
  iteration LONG,
  -- Suite name
  suite_name STRING,
  -- Test name
  test_name STRING,
  -- Type of the measure (sync or async)
  measure_type STRING
) MATERIALIZED AS
SUBPIPELINE measure_slice AS (
  FROM slice AS s
  |> JOIN _chrome_speedometer_3_measure_name AS m
     USING (name)
  |> WHERE s.category = 'blink.user_timing'
  |> SELECT s.ts, s.dur, s.name, m.suite_name, m.test_name, m.measure_type
)
FROM measure_slice AS s
|> JOIN _chrome_speedometer_3_iteration_slice AS i
   ON (s.ts >= i.ts AND s.ts < i.ts + i.dur)
|> SELECT
     s.ts,
     s.dur,
     s.name,
     i.iteration,
     s.suite_name,
     s.test_name,
     s.measure_type
|> ORDER BY s.ts ASC;

-- Slice that covers one Speedometer iteration.
-- The metrics associated are the same ones
-- Speedometer would output, but note we use ns precision (Speedometer uses
-- ~100us) so the actual values might differ a bit.
CREATE PERFETTO PIPELINE chrome_speedometer_3_iteration (
  -- Start timestamp of the iteration
  ts TIMESTAMP,
  -- Duration of the iteration
  dur DURATION,
  -- Iteration name
  name STRING,
  -- Iteration number
  iteration LONG,
  -- Geometric mean of the suite durations for this iteration.
  geomean DOUBLE,
  -- Speedometer score for this iteration (The total score for a run in the
  -- average of all iteration scores).
  score DOUBLE
) MATERIALIZED AS
SUBPIPELINE iteration AS (
  FROM chrome_speedometer_3_measure
  |> AGGREGATE sum(dur / (1000.0 * 1000.0)) AS suite_total
     GROUP BY iteration, suite_name
  -- Compute geometric mean using LN instead of multiplication to prevent
  -- overflows
  |> AGGREGATE exp(avg(ln(suite_total))) AS geomean GROUP BY iteration
)
FROM iteration AS i
|> JOIN _chrome_speedometer_3_iteration_slice AS s
   USING (iteration)
|> SELECT
     s.ts,
     s.dur,
     s.name,
     i.iteration,
     i.geomean,
     1000.0 / i.geomean AS score;

-- Returns the Speedometer 3 score for all iterations in the trace
CREATE PERFETTO FUNCTION chrome_speedometer_3_score()
-- Speedometer 3 score
RETURNS DOUBLE AS
SELECT
  avg(score)
FROM chrome_speedometer_3_iteration;

-- Returns the utid for the main thread that ran Speedometer 3
CREATE PERFETTO FUNCTION chrome_speedometer_3_renderer_main_utid()
-- Renderer main utid
RETURNS LONG AS
WITH
  start_event AS (
    SELECT
      name || '-start' AS name
    FROM _chrome_speedometer_3_measure_name
  )
SELECT
  utid
FROM thread_track
WHERE
  id IN (
    SELECT
      track_id
    FROM slice
    JOIN start_event
      USING (name)
    WHERE
      category = 'blink.user_timing'
  );
