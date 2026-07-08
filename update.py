import re

boundaries_path = "src/trace_processor/perfetto_sql/stdlib/android/cujs/boundaries.sql"
metrics_path = "src/trace_processor/metrics/sql/android/jank/cujs_boundaries.sql"

# 1. Add to stdlib boundaries.sql
with open(boundaries_path, "r") as f:
    b_content = f.read()

render_thread_str = """
-- Similar to `_android_jank_cuj_main_thread_frame_boundary` but for the render
-- thread the expected start time is the time of the first `postAndWait` slice
-- on the main thread.
CREATE PERFETTO TABLE _android_jank_cuj_render_thread_frame_boundary(
  -- CUJ id.
  cuj_id LONG,
  -- Thread id of the render thread.
  utid JOINID(thread.id),
  -- Vsync ID of this frame.
  vsync LONG,
  -- Expected start timestamp (first postAndWait slice).
  ts_expected TIMESTAMP,
  -- Start timestamp of the DrawFrame slice.
  ts_draw_frame_start TIMESTAMP,
  -- End timestamp of the previous DrawFrame slice.
  ts_prev_draw_frame_end TIMESTAMP,
  -- Corrected start timestamp for the frame boundary.
  ts TIMESTAMP,
  -- End timestamp of the DrawFrame slice.
  ts_end TIMESTAMP,
  -- Duration of the frame boundary.
  dur DURATION
)
AS
WITH draw_frame_ordered AS (
  SELECT
    *,
    COALESCE(LAG(ts_end) OVER (PARTITION BY cuj_id ORDER BY vsync ASC, ts ASC), -1) AS ts_prev_draw_frame_end
  FROM _android_jank_cuj_draw_frame_slice
),
frame_boundary_base AS (
  SELECT
    draw_frame.cuj_id,
    draw_frame.utid,
    draw_frame.vsync,
    MIN(post_and_wait.ts) AS ts_expected,
    MIN(draw_frame.ts) AS ts_draw_frame_start,
    MIN(draw_frame.ts_prev_draw_frame_end) AS ts_prev_draw_frame_end,
    MIN(
      MAX(
        MIN(post_and_wait.ts),
        MIN(draw_frame.ts_prev_draw_frame_end)),
      MIN(draw_frame.ts)) AS ts,
    MAX(draw_frame.ts_end) AS ts_end
  FROM draw_frame_ordered draw_frame
  JOIN _android_jank_cuj_do_frames do_frame USING (cuj_id, vsync)
  JOIN descendant_slice(do_frame.id) post_and_wait
  WHERE post_and_wait.name = 'postAndWait'
  GROUP BY draw_frame.cuj_id, draw_frame.utid, draw_frame.vsync
)
SELECT *, ts_end - ts AS dur FROM frame_boundary_base;

-- Compute the CUJ boundary on the render thread from the frame boundaries.
CREATE PERFETTO TABLE _android_jank_cuj_render_thread_cuj_boundary(
  -- CUJ id.
  cuj_id LONG,
  -- Thread id of the render thread.
  utid JOINID(thread.id),
  -- Start timestamp of the CUJ on the render thread.
  ts TIMESTAMP,
  -- End timestamp of the CUJ on the render thread.
  ts_end TIMESTAMP,
  -- Duration of the CUJ on the render thread.
  dur DURATION
)
AS
SELECT
  cuj_id,
  utid,
  MIN(ts) AS ts,
  MAX(ts_end) AS ts_end,
  MAX(ts_end) - MIN(ts) AS dur
FROM _android_jank_cuj_render_thread_frame_boundary
GROUP BY cuj_id, utid;
"""

with open(boundaries_path, "w") as f:
    f.write(b_content + "\n" + render_thread_str)

# 2. Update metrics boundaries
with open(metrics_path, "r") as f:
    m_content = f.read()

# Replace the giant render thread frame boundary definition with shim
p1 = re.compile(r"-- Similar to `android_jank_cuj_main_thread_frame_boundary`(.*?)CREATE PERFETTO TABLE android_jank_cuj_render_thread_frame_boundary AS(.*?)FROM frame_boundary_base;", re.DOTALL)
m_content = p1.sub("DROP TABLE IF EXISTS android_jank_cuj_render_thread_frame_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_render_thread_frame_boundary AS\nSELECT * FROM _android_jank_cuj_render_thread_frame_boundary;", m_content)

p2 = re.compile(r"-- Compute the CUJ boundary on the render thread from the frame boundaries.\nDROP TABLE IF EXISTS android_jank_cuj_render_thread_cuj_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_render_thread_cuj_boundary AS.*?GROUP BY cuj_id, utid;", re.DOTALL)
m_content = p2.sub("DROP TABLE IF EXISTS android_jank_cuj_render_thread_cuj_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_render_thread_cuj_boundary AS\nSELECT * FROM _android_jank_cuj_render_thread_cuj_boundary;", m_content)

with open(metrics_path, "w") as f:
    f.write(m_content)
