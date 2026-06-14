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

INCLUDE PERFETTO MODULE viz.summary.slices;

INCLUDE PERFETTO MODULE viz.summary.threads;

CREATE PERFETTO PIPELINE _process_track_summary MATERIALIZED AS
FROM process_track
|> JOIN _slice_track_summary USING (id)
|> AGGREGATE sum(cnt) AS slice_count GROUP BY upid;

CREATE PERFETTO PIPELINE _heap_profile_allocation_summary MATERIALIZED AS
FROM heap_profile_allocation
|> AGGREGATE count() AS allocation_count GROUP BY upid;

CREATE PERFETTO PIPELINE _heap_profile_graph_summary MATERIALIZED AS
FROM heap_graph_object
|> AGGREGATE count() AS graph_object_count GROUP BY upid;

CREATE PERFETTO PIPELINE _thread_process_grouped_summary MATERIALIZED AS
FROM _thread_available_info_summary
|> JOIN thread USING (utid)
|> WHERE upid IS NOT NULL
|> AGGREGATE
     max(max_running_dur) AS max_running_dur,
     sum(sum_running_dur) AS sum_running_dur,
     sum(running_count) AS running_count,
     sum(slice_count) AS slice_count,
     sum(perf_sample_count) AS perf_sample_count,
     sum(instruments_sample_count) AS instruments_sample_count
   GROUP BY upid;

CREATE PERFETTO PIPELINE _process_available_info_summary MATERIALIZED AS
SUBPIPELINE counter_track_upids AS (
  FROM process_counter_track
  |> SELECT upid
  |> DISTINCT
)
SUBPIPELINE smaps_upids AS (
  FROM profiler_smaps
  |> SELECT upid
  |> DISTINCT
)
FROM process AS p
|> LEFT JOIN _thread_process_grouped_summary AS t_summary USING (upid)
|> LEFT JOIN _process_track_summary AS pt USING (upid)
|> LEFT JOIN _heap_profile_allocation_summary AS hpa USING (upid)
|> LEFT JOIN _heap_profile_graph_summary AS hpg USING (upid)
|> SELECT
     p.upid,
     t_summary.upid AS summary_upid,
     coalesce(t_summary.max_running_dur, 0) AS max_running_dur,
     coalesce(t_summary.sum_running_dur, 0) AS sum_running_dur,
     coalesce(t_summary.running_count, 0) AS running_count,
     coalesce(t_summary.slice_count, 0) AS thread_slice_count,
     coalesce(t_summary.perf_sample_count, 0) AS perf_sample_count,
     coalesce(t_summary.instruments_sample_count, 0) AS instruments_sample_count,
     pt.slice_count AS process_slice_count_raw,
     coalesce(pt.slice_count, 0) AS process_slice_count,
     hpa.allocation_count AS allocation_count_raw,
     coalesce(hpa.allocation_count, 0) AS allocation_count,
     hpg.graph_object_count AS graph_object_count_raw,
     coalesce(hpg.graph_object_count, 0) AS graph_object_count
|> WHERE
     NOT (summary_upid IS NULL
       AND process_slice_count_raw IS NULL
       AND allocation_count_raw IS NULL
       AND graph_object_count_raw IS NULL)
     OR upid IN (FROM counter_track_upids |> SELECT upid)
     OR upid IN (FROM smaps_upids |> SELECT upid)
|> SELECT
     upid,
     max_running_dur,
     sum_running_dur,
     running_count,
     thread_slice_count,
     perf_sample_count,
     instruments_sample_count,
     process_slice_count,
     allocation_count,
     graph_object_count;
