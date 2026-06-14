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

-- NOTE (psqlnext): the single-consumer `_sched_summary`,
-- `_thread_track_summary`, `_perf_sample_summary` and
-- `_instruments_sample_summary` tables are folded into
-- `_thread_available_info_summary` below as inline SUBPIPELINEs.

INCLUDE PERFETTO MODULE viz.summary.slices;

CREATE PERFETTO PIPELINE _thread_available_info_summary MATERIALIZED AS
SUBPIPELINE sched_summary AS (
  FROM sched
  |> WHERE
       NOT (utid IN (FROM thread |> WHERE is_idle |> SELECT utid))
       AND dur != -1
  |> AGGREGATE
       max(dur) AS max_running_dur,
       sum(dur) AS sum_running_dur,
       count() AS running_count
     GROUP BY utid
)
SUBPIPELINE thread_track_summary AS (
  FROM thread_track
  |> JOIN _slice_track_summary USING (id)
  |> AGGREGATE sum(cnt) AS slice_count GROUP BY utid
)
SUBPIPELINE perf_sample_summary AS (
  FROM perf_sample
  |> WHERE callsite_id IS NOT NULL
  |> AGGREGATE count() AS perf_sample_cnt GROUP BY utid
)
SUBPIPELINE instruments_sample_summary AS (
  FROM instruments_sample
  |> WHERE callsite_id IS NOT NULL
  |> AGGREGATE count() AS instruments_sample_cnt GROUP BY utid
)
FROM thread AS t
|> LEFT JOIN sched_summary AS ss USING (utid)
|> LEFT JOIN thread_track_summary AS tts USING (utid)
|> LEFT JOIN perf_sample_summary AS pss USING (utid)
|> LEFT JOIN instruments_sample_summary AS iss USING (utid)
|> SELECT
     t.utid,
     coalesce(ss.max_running_dur, 0) AS max_running_dur,
     coalesce(ss.sum_running_dur, 0) AS sum_running_dur,
     coalesce(ss.running_count, 0) AS running_count,
     coalesce(tts.slice_count, 0) AS slice_count,
     coalesce(pss.perf_sample_cnt, 0) AS perf_sample_count,
     coalesce(iss.instruments_sample_cnt, 0) AS instruments_sample_count,
     ss.max_running_dur AS _raw_max_running_dur,
     ss.sum_running_dur AS _raw_sum_running_dur,
     ss.running_count AS _raw_running_count,
     tts.slice_count AS _raw_slice_count,
     pss.perf_sample_cnt AS _raw_perf_sample_count,
     iss.instruments_sample_cnt AS _raw_instruments_sample_count
|> WHERE
     NOT (_raw_max_running_dur IS NULL
       AND _raw_sum_running_dur IS NULL
       AND _raw_running_count IS NULL
       AND _raw_slice_count IS NULL
       AND _raw_perf_sample_count IS NULL
       AND _raw_instruments_sample_count IS NULL)
     OR utid IN (FROM cpu_profile_stack_sample |> SELECT utid)
     OR utid IN (FROM thread_counter_track |> SELECT utid)
|> SELECT
     utid,
     max_running_dur,
     sum_running_dur,
     running_count,
     slice_count,
     perf_sample_count,
     instruments_sample_count;
