-- Copyright 2025 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

INCLUDE PERFETTO MODULE slices.with_context;

-- Gets the presentation time of the first frame committed after ts
-- in the renderer process with pid.
CREATE PERFETTO FUNCTION _chrome_get_next_presentation_time_by_pid(
    ts TIMESTAMP, pid LONG)
RETURNS TIMESTAMP
AS
SELECT MIN(a.ts + a.dur) AS ts
FROM process_slice s, ancestor_slice(s.id) a
WHERE
  s.name = 'Commit'
  AND a.name = 'PipelineReporter'
  AND s.depth - 1 = a.depth
  AND s.ts > $ts
  AND s.pid = $pid
  -- TODO(crbug.com/409484302): Once we are no longer interested in Chrome
  -- versions <=M136, leave only 'frame_reporter'.
  AND COALESCE(
        EXTRACT_ARG(a.arg_set_id, 'frame_reporter.state'),
        EXTRACT_ARG(a.arg_set_id, 'chrome_frame_reporter.state')
      ) = 'STATE_PRESENTED_ALL';

-- User timing trace events can be emitted by either performance.mark() or
-- performance.measure(). The former appear on the CrRendererMain thread track,
-- the latter on their own custom track inside the Renderer process.
-- This query looks for the event track info in both thread_track and
-- process_track to support both cases.
CREATE PERFETTO PIPELINE _chrome_loadline2_marks_with_pid (
  -- Mark timestamp
  ts TIMESTAMP,
  -- Name of the page
  page STRING,
  -- Name of the mark
  mark STRING,
  -- PID of the Renderer process
  pid LONG
) AS
FROM slice s
|> LEFT JOIN thread_track tt ON s.track_id = tt.id
|> LEFT JOIN process_track pt ON s.track_id = pt.id
|> LEFT JOIN thread t ON tt.utid = t.utid
|> JOIN process p ON p.upid = COALESCE(t.upid, pt.upid)
|> WHERE s.category = 'blink.user_timing' AND s.name GLOB 'LoadLine2/*/*'
|> SELECT
     ts,
     STR_SPLIT(s.name, '/', 1) AS page,
     STR_SPLIT(s.name, '/', 2) AS mark,
     pid;

-- All LoadLine2 stages per page
CREATE PERFETTO PIPELINE chrome_loadline2_stages (
  -- Name of the page
  page STRING,
  -- Story start timestamp
  story_start TIMESTAMP,
  -- Start request timestamp
  start_request TIMESTAMP,
  -- End request timestamp
  end_request TIMESTAMP,
  -- Renderer ready timestamp
  renderer_ready TIMESTAMP,
  -- Visual mark timestamp
  visual_mark TIMESTAMP,
  -- Visual rAF timestamp
  visual_raf TIMESTAMP,
  -- Visual presentation timestamp
  visual_presentation TIMESTAMP,
  -- Interactive mark timestamp
  interactive_mark TIMESTAMP,
  -- Interactive rAF timestamp
  interactive_raf TIMESTAMP,
  -- Interactive presentation timestamp
  interactive_presentation TIMESTAMP,
  -- Story finish timestamp
  story_finish TIMESTAMP
) AS
-- Story start and Renderer pid for each page.
SUBPIPELINE story_start_with_pid AS (
  SUBPIPELINE story_pid AS (
    FROM _chrome_loadline2_marks_with_pid
    |> WHERE mark = 'finish'
    |> SELECT page, pid
  )
  FROM _chrome_loadline2_marks_with_pid
  |> WHERE mark = 'start'
  |> SELECT page, ts AS story_start
  |> JOIN story_pid USING (page)
)
-- Start timestamp for the first network request for each page.
SUBPIPELINE start_request AS (
  FROM slice
  |> JOIN story_start_with_pid AS ss ON slice.ts >= ss.story_start
  |> WHERE slice.name = 'WillStartRequest'
  |> AGGREGATE MIN(slice.ts) AS start_request GROUP BY ss.page
)
-- Finish timestamp for the first network request for each page.
SUBPIPELINE end_request AS (
  FROM slice
  |> JOIN story_start_with_pid AS ss ON slice.ts >= ss.story_start
  |> WHERE slice.name = 'CommitSentToFirstSubresourceLoadStart'
  |> AGGREGATE MIN(slice.ts) AS end_request GROUP BY ss.page
)
-- Renderer ready for each page.
SUBPIPELINE renderer_ready AS (
  FROM thread_slice
  |> JOIN story_start_with_pid AS ss USING (pid)
  |> WHERE thread_slice.name = 'DocumentLoader::CommitNavigation'
       AND thread_slice.ts >= ss.story_start
  |> AGGREGATE MIN(thread_slice.ts) AS renderer_ready GROUP BY ss.page
)
-- Visual mark for each page.
SUBPIPELINE visual_mark AS (
  FROM _chrome_loadline2_marks_with_pid
  |> WHERE mark = 'visual'
  |> SELECT page, ts AS visual_mark, pid
)
-- Timestamp of the second rAF after visual mark for each page.
SUBPIPELINE visual_raf AS (
  FROM _chrome_loadline2_marks_with_pid
  |> WHERE mark = 'visual_raf'
  |> SELECT page, ts AS visual_raf
)
-- Visual presentation for each page.
SUBPIPELINE visual_presentation AS (
  FROM visual_mark
  |> SELECT
       page,
       _chrome_get_next_presentation_time_by_pid(visual_mark, pid)
         AS visual_presentation
)
-- Interactive mark for each page.
SUBPIPELINE interactive_mark AS (
  FROM _chrome_loadline2_marks_with_pid
  |> WHERE mark = 'interactive'
  |> SELECT page, ts AS interactive_mark, pid
)
-- Timestamp of the second rAF after interactive mark for each page.
SUBPIPELINE interactive_raf AS (
  FROM _chrome_loadline2_marks_with_pid
  |> WHERE mark = 'interactive_raf'
  |> SELECT page, ts AS interactive_raf
)
-- Interactive presentation for each page.
SUBPIPELINE interactive_presentation AS (
  FROM interactive_mark
  |> SELECT
       page,
       _chrome_get_next_presentation_time_by_pid(interactive_mark, pid)
         AS interactive_presentation
)
-- Story finish for each page.
SUBPIPELINE story_finish AS (
  FROM _chrome_loadline2_marks_with_pid
  |> WHERE mark = 'finish'
  |> SELECT page, ts AS story_finish
)
FROM story_start_with_pid
|> LEFT JOIN start_request USING (page)
|> LEFT JOIN end_request USING (page)
|> LEFT JOIN renderer_ready USING (page)
|> LEFT JOIN visual_mark USING (page)
|> LEFT JOIN visual_raf USING (page)
|> LEFT JOIN visual_presentation USING (page)
|> LEFT JOIN interactive_mark USING (page)
|> LEFT JOIN interactive_raf USING (page)
|> LEFT JOIN interactive_presentation USING (page)
|> LEFT JOIN story_finish USING (page)
|> SELECT
     page,
     story_start,
     start_request,
     end_request,
     renderer_ready,
     visual_mark,
     visual_raf,
     visual_presentation,
     interactive_mark,
     interactive_raf,
     interactive_presentation,
     story_finish;
