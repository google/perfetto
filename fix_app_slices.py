metrics_path = "src/trace_processor/metrics/sql/android/jank/slices.sql"
with open(metrics_path, "r") as f:
    text = f.read()

text = text.replace("""DROP VIEW IF EXISTS android_jank_cuj_slice;
CREATE PERFETTO VIEW android_jank_cuj_slice AS
SELECT
  cuj_id,
  process.upid,
  process.name AS process_name,
  thread.utid,
  thread.name AS thread_name,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_boundary boundary
JOIN process USING (upid)
JOIN thread USING (upid)
JOIN thread_track USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;""", """DROP VIEW IF EXISTS android_jank_cuj_slice;
CREATE PERFETTO VIEW android_jank_cuj_slice AS
SELECT * FROM _android_jank_cuj_slice;""")

text = text.replace("""DROP TABLE IF EXISTS android_jank_cuj_main_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_main_thread_slice AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_main_thread_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;""", """DROP TABLE IF EXISTS android_jank_cuj_main_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_main_thread_slice AS
SELECT * FROM _android_jank_cuj_main_thread_slice;""")

text = text.replace("""DROP TABLE IF EXISTS android_jank_cuj_render_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_slice AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_render_thread_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;""", """DROP TABLE IF EXISTS android_jank_cuj_render_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_slice AS
SELECT * FROM _android_jank_cuj_render_thread_slice;""")

with open(metrics_path, "w") as f:
    f.write(text)
