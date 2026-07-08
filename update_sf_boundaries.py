import re

boundaries_path = "src/trace_processor/perfetto_sql/stdlib/android/cujs/boundaries.sql"
metrics_path = "src/trace_processor/metrics/sql/android/jank/cujs_boundaries.sql"

# 1. Add to stdlib boundaries.sql
with open(boundaries_path, "r") as f:
    b_content = f.read()

sf_boundaries_str = """
-- Similarly, extract the min/max vsync for the SF from
-- commit/compose/onMessageInvalidate slices on its main thread.
CREATE PERFETTO TABLE _android_jank_cuj_sf_vsync_boundary(
  -- CUJ id.
  cuj_id LONG,
  -- Minimum vsync ID within the CUJ.
  vsync_min LONG,
  -- Maximum vsync ID within the CUJ.
  vsync_max LONG
)
AS
SELECT
  cuj_id,
  MIN(vsync) AS vsync_min,
  MAX(vsync) AS vsync_max
FROM _android_jank_cuj_sf_root_slice
GROUP BY cuj_id;

-- Compute the CUJ boundary on the main thread from the frame boundaries.
CREATE PERFETTO TABLE _android_jank_cuj_sf_main_thread_cuj_boundary(
  -- CUJ id.
  cuj_id LONG,
  -- Thread id of the SF main thread.
  utid JOINID(thread.id),
  -- Start timestamp of the CUJ on the SF main thread.
  ts TIMESTAMP,
  -- End timestamp of the CUJ on the SF main thread.
  ts_end TIMESTAMP,
  -- Duration of the CUJ on the SF main thread.
  dur DURATION
)
AS
SELECT
  cuj_id,
  utid,
  MIN(ts) AS ts,
  MAX(ts_end) AS ts_end,
  MAX(ts_end) - MIN(ts) AS dur
FROM _android_jank_cuj_sf_main_thread_frame_boundary
GROUP BY cuj_id, utid;

-- RenderEngine will only work on a frame if SF falls back to client composition.
CREATE PERFETTO TABLE _android_jank_cuj_sf_render_engine_frame_boundary(
  -- CUJ id.
  cuj_id LONG,
  -- Thread id of the RenderEngine thread.
  utid JOINID(thread.id),
  -- Vsync ID of this frame.
  vsync LONG,
  -- Timestamp of the composeSurfaces slice bounding this drawLayers.
  ts TIMESTAMP,
  -- Start timestamp of the drawLayers slice.
  ts_draw_layers_start TIMESTAMP,
  -- End timestamp of the drawLayers slice.
  ts_end TIMESTAMP,
  -- Duration of the frame boundary.
  dur DURATION
)
AS
SELECT
  cuj_id,
  utid,
  vsync,
  draw_layers.ts_compose_surfaces AS ts,
  draw_layers.ts AS ts_draw_layers_start,
  draw_layers.ts_end,
  draw_layers.ts_end - draw_layers.ts_compose_surfaces AS dur
FROM _android_jank_cuj_sf_draw_layers_slice draw_layers;

CREATE PERFETTO TABLE _android_jank_cuj_sf_boundary(
  -- CUJ id.
  cuj_id LONG,
  -- Start timestamp of the CUJ.
  ts TIMESTAMP,
  -- End timestamp of the CUJ.
  ts_end TIMESTAMP,
  -- Duration of the CUJ.
  dur DURATION
)
AS
SELECT cuj_id, ts, ts_end, dur
FROM _android_jank_cuj_sf_main_thread_cuj_boundary;
"""
with open(boundaries_path, "w") as f:
    f.write(b_content.replace('INCLUDE PERFETTO MODULE android.cujs.base;', 'INCLUDE PERFETTO MODULE android.cujs.base;\nINCLUDE PERFETTO MODULE android.surfaceflinger;\nINCLUDE PERFETTO MODULE android.cujs.relevant_slices;\n') + sf_boundaries_str)

# 2. Update metrics boundaries
with open(metrics_path, "r") as f:
    m_content = f.read()

m_content = m_content.replace("""-- Similarly, extract the min/max vsync for the SF from
-- commit/compose/onMessageInvalidate slices on its main thread.
DROP TABLE IF EXISTS android_jank_cuj_sf_vsync_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_vsync_boundary AS
SELECT
  cuj_id,
  MIN(vsync) AS vsync_min,
  MAX(vsync) AS vsync_max
FROM android_jank_cuj_sf_root_slice
GROUP BY cuj_id;""", """DROP TABLE IF EXISTS android_jank_cuj_sf_vsync_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_vsync_boundary AS
SELECT * FROM _android_jank_cuj_sf_vsync_boundary;""")

m_content = re.sub(r"-- Similar to `android_jank_cuj_main_thread_frame_boundary`, calculates.*?ON main_thread_slice\.vsync = CAST\(expected_timeline\.name AS INTEGER\);", 
"DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_frame_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_frame_boundary AS\nSELECT * FROM _android_jank_cuj_sf_main_thread_frame_boundary;", m_content, flags=re.DOTALL)

m_content = re.sub(r"-- Compute the CUJ boundary on the main thread from the frame boundaries.\nDROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_cuj_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_cuj_boundary AS.*?GROUP BY cuj_id, utid;", "DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_cuj_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_cuj_boundary AS\nSELECT * FROM _android_jank_cuj_sf_main_thread_cuj_boundary;", m_content, flags=re.DOTALL)

m_content = re.sub(r"-- RenderEngine will only work on a frame if SF falls back to client composition.*?FROM android_jank_cuj_sf_draw_layers_slice draw_layers;", "DROP TABLE IF EXISTS android_jank_cuj_sf_render_engine_frame_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_sf_render_engine_frame_boundary AS\nSELECT * FROM _android_jank_cuj_sf_render_engine_frame_boundary;", m_content, flags=re.DOTALL)

m_content = re.sub(r"DROP TABLE IF EXISTS android_jank_cuj_sf_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_sf_boundary AS\nSELECT cuj_id, ts, ts_end, dur\nFROM android_jank_cuj_sf_main_thread_cuj_boundary;", "DROP TABLE IF EXISTS android_jank_cuj_sf_boundary;\nCREATE PERFETTO TABLE android_jank_cuj_sf_boundary AS\nSELECT * FROM _android_jank_cuj_sf_boundary;", m_content, flags=re.DOTALL)

with open(metrics_path, "w") as f:
    f.write(m_content)
