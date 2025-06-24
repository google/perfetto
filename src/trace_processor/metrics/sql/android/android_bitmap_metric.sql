--
-- Copyright 2025 The Android Open Source Project
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

DROP VIEW IF EXISTS bitmap_metric_span;
CREATE PERFETTO VIEW bitmap_metric_span AS
SELECT
  ts,
  LEAD(ts, 1, (
    SELECT IFNULL(
      end_ts,
      trace_end()
    )
    FROM process p WHERE p.upid = pct.upid) + 1
  ) OVER(PARTITION BY track_id ORDER BY ts) - ts AS dur,
  upid,
  value AS metric_val,
  name AS metric_name
FROM counter c JOIN process_counter_track pct
  ON pct.id = c.track_id
WHERE name = 'Bitmap Count' OR name = 'Bitmap Memory' AND upid IS NOT NULL;

DROP TABLE IF EXISTS bitmap_metric_stats;
CREATE PERFETTO TABLE bitmap_metric_stats AS
SELECT
  span.metric_name,
  upid,
  MIN(span.metric_val) AS min_value,
  MAX(span.metric_val) AS max_value,
  SUM(span.metric_val * span.dur) / SUM(span.dur) AS avg_value
FROM bitmap_metric_span AS span
WHERE upid IS NOT NULL
GROUP BY 1,2
ORDER BY 1;

DROP TABLE IF EXISTS filtered_processes_with_bitmap_metrics;
CREATE PERFETTO TABLE filtered_processes_with_bitmap_metrics AS
SELECT p.upid, p.name
FROM process p WHERE p.upid IN
  (SELECT DISTINCT upid FROM bitmap_metric_stats);


DROP VIEW IF EXISTS android_bitmap_metric_output;
CREATE PERFETTO VIEW android_bitmap_metric_output AS
SELECT AndroidBitmapMetric(
    'process_with_bitmaps', (
        SELECT RepeatedField(
            AndroidBitmapMetric_ProcessWithBitmaps(
                'process_name', p.name,
                'counters', (
                    SELECT RepeatedField(
                        AndroidBitmapMetric_BitmapCounter(
                            'name', stats.metric_name,
                            'min', stats.min_value,
                            'max', stats.max_value,
                            'avg', stats.avg_value
                        )
                    ) FROM bitmap_metric_stats stats where stats.upid = p.upid
                )
            )
        ) FROM filtered_processes_with_bitmap_metrics p
    )
);
