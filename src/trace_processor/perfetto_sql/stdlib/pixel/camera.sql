--
-- Copyright 2024 The Android Open Source Project
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

-- Break down camera Camera graph execution slices per node, port group, and frame.
-- This table extracts key identifiers from Camera graph execution slice names and
-- provides timing information for each processing stage.
CREATE PERFETTO PIPELINE pixel_camera_frames(
  -- Unique identifier for this slice.
  id ID(slice.id),
  -- Start timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice execution.
  dur DURATION,
  -- Track ID for this slice.
  track_id JOINID(track.id),
  -- Thread ID (utid) executing this slice.
  utid JOINID(thread.id),
  -- Name of the thread executing this slice.
  thread_name STRING,
  -- Name of the processing node in the Camera graph.
  node STRING,
  -- Port group name for the node.
  port_group STRING,
  -- Frame number being processed.
  frame_number LONG,
  -- Camera ID associated with this slice.
  cam_id LONG
) MATERIALIZED AS
FROM thread_slice
-- Only include slices matching the Camera graph pattern and with valid durations.
|> WHERE name GLOB 'cam*_*:* (frame *)' AND dur != -1
|> SELECT
     id,
     ts,
     dur,
     track_id,
     utid,
     thread_name,
     -- Slices follow the pattern "camX_Y:Z (frame N)" where X is the camera ID,
     -- Y is the node name, Z is the port group, and N is the frame number.
     substr(str_split(name, ':', 0), 6) AS node,
     str_split(str_split(name, ':', 1), ' (', 0) AS port_group,
     cast_int!(STR_SPLIT(STR_SPLIT(name, '(frame', 1), ')', 0)) AS frame_number,
     cast_int!(STR_SPLIT(STR_SPLIT(name, 'cam', 1), '_', 0)) AS cam_id;

-- Process memory and DMA heap usage timeline for Pixel Camera processes (GCA, HAL, CameraServer).
CREATE PERFETTO PIPELINE pixel_camera_memory_span(
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration of the span.
  dur DURATION,
  -- RSS of GoogleCamera process.
  gca_rss LONG,
  -- RSS of Camera HAL process (camera.provider).
  hal_rss LONG,
  -- RSS of CameraServer process.
  cameraserver_rss LONG,
  -- Value of DMA heap counters.
  dma LONG,
  -- Sum of all camera RSS and DMA heap memory values.
  rss_and_dma LONG
) MATERIALIZED AS
-- RSS of GoogleCamera application.
SUBPIPELINE rss_gca AS (
  FROM memory_rss_and_swap_per_process
  |> JOIN (
       FROM process
       |> JOIN (
            FROM memory_rss_and_swap_per_process
            |> AGGREGATE
                 max(coalesce(file_rss, 0) + coalesce(anon_rss, 0) + coalesce(shmem_rss, 0)) AS rss
               GROUP BY upid
          ) USING (upid)
       |> WHERE name GLOB '*GoogleCamera'
            OR name GLOB '*googlecamera.fishfood'
            OR name GLOB '*GoogleCameraEng'
       |> AGGREGATE max(rss) AS rss GROUP BY upid
       |> LIMIT 1
     ) USING (upid)
  |> SELECT ts, dur, coalesce(file_rss, 0) + coalesce(anon_rss, 0) + coalesce(shmem_rss, 0) AS gca_rss_val
)
-- RSS of camera HAL.
SUBPIPELINE rss_camera_hal AS (
  FROM memory_rss_and_swap_per_process
  |> JOIN (
       FROM process
       |> WHERE name GLOB '*camera.provider*'
       |> AGGREGATE max(start_ts) AS start_ts GROUP BY upid
       |> LIMIT 1
     ) USING (upid)
  |> SELECT ts, dur, coalesce(file_rss, 0) + coalesce(anon_rss, 0) + coalesce(shmem_rss, 0) AS hal_rss_val
)
-- RSS of cameraserver.
SUBPIPELINE rss_cameraserver AS (
  FROM memory_rss_and_swap_per_process
  |> JOIN (
       FROM process
       |> WHERE name GLOB '*cameraserver'
       |> AGGREGATE max(start_ts) AS start_ts GROUP BY upid
       |> LIMIT 1
     ) USING (upid)
  |> SELECT ts, dur, coalesce(file_rss, 0) + coalesce(anon_rss, 0) + coalesce(shmem_rss, 0) AS cameraserver_rss_val
)
-- Spans of DMA heap usage.
SUBPIPELINE dma_span AS (
  FROM counter AS c
  |> JOIN counter_track AS t ON t.id = c.track_id
  |> WHERE _counter_track_is_only_name_dimension(t.id) AND t.name = 'mem.dma_heap'
  |> SELECT
       c.ts,
       LEAD(c.ts, 1, trace_end()) OVER (PARTITION BY c.track_id ORDER BY c.ts) - c.ts AS dur,
       c.value AS dma_val
)
INTERVAL UNION OF (dma_span AS d, rss_gca AS g, rss_camera_hal AS h, rss_cameraserver AS s)
|> SELECT
     ts,
     dur,
     cast_int!(IFNULL(g.gca_rss_val, 0)) AS gca_rss,
     cast_int!(IFNULL(h.hal_rss_val, 0)) AS hal_rss,
     cast_int!(IFNULL(s.cameraserver_rss_val, 0)) AS cameraserver_rss,
     cast_int!(IFNULL(d.dma_val, 0)) AS dma,
     cast_int!(IFNULL(g.gca_rss_val, 0) + IFNULL(h.hal_rss_val, 0)
       + IFNULL(s.cameraserver_rss_val, 0)
       + IFNULL(d.dma_val, 0)) AS rss_and_dma;
