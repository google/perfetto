-- A simple table that checks the time between VSync (this can be used to
-- determine if we're refreshing at 90 FPS or 60 FPS).
--
-- Note: In traces without the "Java" category there will be no VSync
--       TraceEvents and this table will be empty.
DROP TABLE IF EXISTS vsync_intervals;
CREATE TABLE vsync_intervals AS
SELECT
  slice_id,
  ts,
  dur,
  track_id,
  LEAD(ts) OVER(PARTITION BY track_id ORDER BY ts) - ts AS time_to_next_vsync
FROM slice
WHERE name = "VSync"
ORDER BY track_id, ts;

SELECT CREATE_FUNCTION(
  -- Function: compute the average Vysnc interval of the
  -- gesture (hopefully this would be either 60 FPS for the whole gesture or 90
  -- FPS but that isn't always the case) on the given time segment.
  -- If the trace doesn't contain the VSync TraceEvent we just fall back on
  -- assuming its 60 FPS (this is the 1.6e+7 in the COALESCE which
  -- corresponds to 16 ms or 60 FPS).
  --
  -- begin_ts: segment start time
  -- end_ts: segment end time
  'CalculateAvgVsyncInterval(begin_ts LONG, end_ts LONG)',
  -- Returns: the average Vysnc interval on this time segment
  -- or 1.6e+7, if trace doesn't contain the VSync TraceEvent.
  'FLOAT',
  'SELECT
    COALESCE((
      SELECT
        CAST(AVG(time_to_next_vsync) AS FLOAT)
      FROM vsync_intervals in_query
      WHERE
        time_to_next_vsync IS NOT NULL AND
        in_query.ts > $begin_ts AND
        in_query.ts < $end_ts
    ), 1e+9 / 60)'
);
