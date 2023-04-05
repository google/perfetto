#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Chrome(DiffTestModule):

  def test_scroll_jank_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_jank_general_validation_test.sql'),
        out=Path('scroll_jank_general_validation.out'))

  def test_scroll_jank(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank.sql');

SELECT
  gesture_scroll_id,
  trace_id,
  jank,
  ts,
  dur,
  jank_budget
FROM scroll_jank;
""",
        out=Path('scroll_jank.out'))

  def test_event_latency_to_breakdowns(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query="""
SELECT RUN_METRIC('chrome/event_latency_to_breakdowns.sql');

SELECT
  event_latency_ts,
  event_latency_dur,
  event_type,
  GenerationToRendererCompositorNs,
  GenerationToBrowserMainNs,
  BrowserMainToRendererCompositorNs,
  RendererCompositorQueueingDelayNs,
  unknown_stages_seen
FROM event_latency_to_breakdowns
ORDER BY event_latency_id
LIMIT 30;
""",
        out=Path('event_latency_to_breakdowns.out'))

  def test_event_latency_scroll_jank(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query="""
SELECT RUN_METRIC('chrome/event_latency_scroll_jank.sql');

SELECT
  jank,
  next_jank,
  prev_jank,
  gesture_begin_ts,
  gesture_end_ts,
  ts,
  dur,
  event_type,
  next_ts,
  next_dur,
  prev_ts,
  prev_dur
FROM scroll_event_latency_jank
ORDER BY jank DESC
LIMIT 10;
""",
        out=Path('event_latency_scroll_jank.out'))

  def test_event_latency_scroll_jank_cause(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query="""
SELECT RUN_METRIC('chrome/event_latency_scroll_jank_cause.sql');

SELECT
  dur,
  ts,
  event_type,
  next_jank,
  prev_jank,
  next_delta_dur_ns,
  prev_delta_dur_ns,
  cause_of_jank,
  max_delta_dur_ns,
  sub_cause_of_jank
FROM event_latency_scroll_jank_cause
ORDER by ts;
""",
        out=Path('event_latency_scroll_jank_cause.out'))

  def test_scroll_flow_event(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_flow_event.sql');

SELECT
  trace_id,
  ts,
  dur,
  jank,
  step,
  ancestor_end,
  maybe_next_ancestor_ts,
  next_ts,
  next_trace_id,
  next_step
FROM scroll_flow_event
ORDER BY gesture_scroll_id, trace_id, ts;
""",
        out=Path('scroll_flow_event.out'))

  def test_scroll_flow_event_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_flow_event.sql');

SELECT
  -- Each trace_id (in our example trace not true in general) has 8 steps. There
  -- are 139 scrolls. So we expect 1112 rows in total 72 of which are janky.
  (
    SELECT
      COUNT(*)
    FROM (
      SELECT
        trace_id,
        COUNT(*)
      FROM scroll_flow_event
      GROUP BY trace_id
    )
  ) AS total_scroll_updates,
  (
    SELECT COUNT(*) FROM scroll_flow_event
  ) AS total_flow_event_steps,
  (
    SELECT COUNT(*) FROM scroll_flow_event WHERE jank
  ) AS total_janky_flow_event_steps,
  (
    SELECT COUNT(*) FROM (SELECT step FROM scroll_flow_event GROUP BY step)
  ) AS number_of_unique_steps;
""",
        out=Path('scroll_flow_event_general_validation.out'))

  def test_scroll_jank_cause(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank_cause.sql');

SELECT
  COUNT(*) AS total,
  SUM(jank) AS total_jank,
  SUM(explained_jank + unexplained_jank) AS sum_explained_and_unexplained,
  SUM(
    CASE WHEN explained_jank THEN
      unexplained_jank
      ELSE
        CASE WHEN jank AND NOT unexplained_jank THEN
          1
          ELSE
            0
        END
    END
  ) AS error_rows
FROM scroll_jank_cause;
""",
        out=Csv("""
"total","total_jank","sum_explained_and_unexplained","error_rows"
139,7,7,0
"""))

  def test_scroll_flow_event_queuing_delay(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_flow_event_queuing_delay.sql');

SELECT
  trace_id,
  jank,
  step,
  next_step,
  ancestor_end,
  maybe_next_ancestor_ts,
  queuing_time_ns
FROM scroll_flow_event_queuing_delay
WHERE trace_id = 2954 OR trace_id = 2956 OR trace_id = 2960
ORDER BY trace_id, ts;
""",
        out=Path('scroll_flow_event_queuing_delay.out'))

  def test_scroll_flow_event_general_validation_2(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path(
            'scroll_flow_event_queuing_delay_general_validation_test.sql'),
        out=Path('scroll_flow_event_general_validation.out'))

  def test_scroll_jank_cause_queuing_delay(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank_cause_queuing_delay.sql');

SELECT
  process_name,
  thread_name,
  trace_id,
  jank,
  dur_overlapping_ns,
  metric_name
FROM scroll_jank_cause_queuing_delay
WHERE trace_id = 2918 OR trace_id = 2926
ORDER BY trace_id ASC, ts ASC;
""",
        out=Path('scroll_jank_cause_queuing_delay.out'))

  def test_scroll_jank_cause_queuing_delay_restricted(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank_cause_queuing_delay.sql');

SELECT
  process_name,
  thread_name,
  trace_id,
  jank,
  dur_overlapping_ns,
  restricted_metric_name
FROM scroll_jank_cause_queuing_delay
WHERE trace_id = 2918 OR trace_id = 2926
ORDER BY trace_id ASC, ts ASC;
""",
        out=Path('scroll_jank_cause_queuing_delay_restricted.out'))

  def test_scroll_jank_cause_queuing_delay_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank_cause_queuing_delay.sql');

SELECT
  COUNT(*) AS total,
  (
    SELECT DISTINCT
      (avg_no_jank_dur_overlapping_ns)
    FROM scroll_jank_cause_queuing_delay
    WHERE
      location = "LatencyInfo.Flow"
      AND jank
  ) AS janky_latency_info_non_jank_avg_dur,
  (
    SELECT DISTINCT
      (avg_no_jank_dur_overlapping_ns)
    FROM scroll_jank_cause_queuing_delay
    WHERE
      location = "LatencyInfo.Flow"
      AND NOT jank
  ) AS non_janky_latency_info_non_jank_avg_dur
FROM (
  SELECT
    trace_id
  FROM scroll_jank_cause_queuing_delay
  GROUP BY trace_id
);
""",
        out=Path('scroll_jank_cause_queuing_delay_general_validation.out'))

  def test_chrome_thread_slice(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_thread_slice.sql');

SELECT
  EXTRACT_ARG(arg_set_id, 'chrome_latency_info.trace_id') AS trace_id,
  dur,
  thread_dur
FROM chrome_thread_slice
WHERE
  name = 'LatencyInfo.Flow'
  AND EXTRACT_ARG(arg_set_id, 'chrome_latency_info.trace_id') = 2734;
""",
        out=Csv("""
"trace_id","dur","thread_dur"
2734,25000,25000
2734,1000,2000
2734,2000,2000
2734,258000,171000
2734,1000,1000
"""))

  def test_chrome_input_to_browser_intervals(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../data/scrolling_with_blocked_nonblocked_frames.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_input_to_browser_intervals.sql');

SELECT
  *
FROM chrome_input_to_browser_intervals
WHERE window_start_ts >= 60934320005158
  AND window_start_ts <= 60934338798158;
""",
        out=Path('chrome_input_to_browser_intervals.out'))

  def test_chrome_scroll_jank_caused_by_scheduling_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fling_with_input_delay.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_scroll_jank_caused_by_scheduling.sql',
  'dur_causes_jank_ms',
/* dur_causes_jank_ms = */ '5');

SELECT
  full_name,
  total_duration_ms,
  total_thread_duration_ms,
  count,
  window_start_ts,
  window_end_ts,
  scroll_type
FROM chrome_scroll_jank_caused_by_scheduling;
""",
        out=Path('chrome_scroll_jank_caused_by_scheduling_test.out'))

  def test_chrome_tasks_delaying_input_processing_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fling_with_input_delay.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_tasks_delaying_input_processing.sql',
  'duration_causing_jank_ms',
 /* duration_causing_jank_ms = */ '8');

SELECT
  full_name,
  duration_ms,
  thread_dur_ms
FROM chrome_tasks_delaying_input_processing;
""",
        out=Path('chrome_tasks_delaying_input_processing_test.out'))

  def test_long_task_tracking_trace_chrome_long_tasks_delaying_input_processing_test(
      self):
    return DiffTestBlueprint(
        trace=Path('../../data/long_task_tracking_trace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_long_tasks_delaying_input_processing.sql');

SELECT
  full_name,
  duration_ms,
  slice_id
FROM chrome_tasks_delaying_input_processing
ORDER BY slice_id;
""",
        out=Path(
            'long_task_tracking_trace_chrome_long_tasks_delaying_input_processing_test.out'
        ))

  def test_experimental_reliable_chrome_tasks_delaying_input_processing_test(
      self):
    return DiffTestBlueprint(
        trace=Path('../../data/fling_with_input_delay.pftrace'),
        query="""
SELECT RUN_METRIC(
    'chrome/experimental_reliable_chrome_tasks_delaying_input_processing.sql',
    'duration_causing_jank_ms', '8');

SELECT
  full_name,
  duration_ms,
  thread_dur_ms
FROM chrome_tasks_delaying_input_processing;
""",
        out=Path(
            'experimental_reliable_chrome_tasks_delaying_input_processing_test.out'
        ))

  def test_chrome_scroll_inputs_per_frame_test(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../data/scrolling_with_blocked_nonblocked_frames.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_scroll_inputs_per_frame.sql');

SELECT
  count_for_frame,
  ts
FROM chrome_scroll_inputs_per_frame
WHERE ts = 60934316798158;
""",
        out=Csv("""
"count_for_frame","ts"
4,60934316798158
"""))

  def test_chrome_thread_slice_repeated(self):
    return DiffTestBlueprint(
        trace=Path('../track_event/track_event_counters.textproto'),
        query="""
SELECT RUN_METRIC('chrome/chrome_thread_slice.sql');

SELECT
  name,
  ts,
  dur,
  thread_dur
FROM chrome_thread_slice;
""",
        out=Csv("""
"name","ts","dur","thread_dur"
"event1_on_t1",1000,100,10000
"event2_on_t1",2000,200,30000
"event3_on_t1",2000,200,10000
"event4_on_t1",4000,0,0
"float_counter_on_t1",4300,0,"[NULL]"
"float_counter_on_t1",4500,0,"[NULL]"
"event1_on_t3",4000,100,5000
"""))

  def test_frame_times_metric(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_rendering_desktop.pftrace'),
        query=Metric('frame_times'),
        out=Path('frame_times_metric.out'))

  def test_chrome_dropped_frames_metric(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_rendering_desktop.pftrace'),
        query=Metric('chrome_dropped_frames'),
        out=TextProto(r"""
[perfetto.protos.chrome_dropped_frames]: {
  dropped_frame: {
    ts: 166479338462000
    process_name: "Renderer"
    pid: 12743
  }
  dropped_frame: {
    ts: 166479355302000
    process_name: "Renderer"
    pid: 12743
  }
}"""))

  def test_chrome_long_latency_metric(self):
    return DiffTestBlueprint(
        trace=Path('../chrome/long_event_latency.textproto'),
        query="""
SELECT RUN_METRIC('experimental/chrome_long_latency.sql');

SELECT * FROM long_latency_with_process_info;
""",
        out=Csv("""
"ts","event_type","process_name","process_id"
200111000,"FirstGestureScrollUpdate,GestureScrollUpdate","Renderer",1001
200111000,"GestureScrollUpdate","Renderer",1002
280111001,"GestureScrollUpdate","Renderer",1001
"""))

  def test_scroll_jank_mojo_simple_watcher(self):
    return DiffTestBlueprint(
        trace=Path('scroll_jank_mojo_simple_watcher.py'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank_cause_queuing_delay.sql');

SELECT
  trace_id,
  jank,
  dur_overlapping_ns,
  metric_name
FROM scroll_jank_cause_queuing_delay
ORDER BY trace_id ASC, ts ASC;
""",
        out=Path('scroll_jank_mojo_simple_watcher.out'))

  def test_scroll_jank_gpu_check(self):
    return DiffTestBlueprint(
        trace=Path('scroll_jank_gpu_check.py'),
        query="""
SELECT RUN_METRIC('chrome/scroll_jank.sql');

SELECT ts, jank
FROM scroll_jank
ORDER BY ts ASC;
""",
        out=Csv("""
"ts","jank"
15000000,0
30000000,1
115000000,0
"""))

  def test_touch_jank(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_touch_gesture_scroll.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/touch_jank.sql');

SELECT
  touch_id,
  trace_id,
  jank,
  ts,
  dur,
  jank_budget
FROM touch_jank;
""",
        out=Path('touch_jank.out'))

  def test_touch_flow_event(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_touch_gesture_scroll.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/touch_flow_event.sql');

SELECT
  trace_id,
  ts,
  dur,
  jank,
  step,
  ancestor_end,
  maybe_next_ancestor_ts,
  next_ts,
  next_trace_id,
  next_step
FROM touch_flow_event
ORDER BY touch_id, trace_id, ts;
""",
        out=Path('touch_flow_event.out'))

  def test_touch_flow_event_queuing_delay(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_touch_gesture_scroll.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/touch_flow_event_queuing_delay.sql');

SELECT
  trace_id,
  jank,
  step,
  next_step,
  ancestor_end,
  maybe_next_ancestor_ts,
  queuing_time_ns
FROM touch_flow_event_queuing_delay
WHERE trace_id = 6915 OR trace_id = 6911 OR trace_id = 6940
ORDER BY trace_id, ts;
""",
        out=Path('touch_flow_event_queuing_delay.out'))

  def test_touch_jank_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query="""
SELECT RUN_METRIC('chrome/touch_jank.sql');

SELECT
  touch_id,
  trace_id,
  jank,
  ts,
  dur,
  jank_budget
FROM touch_jank;
""",
        out=Csv("""
"touch_id","trace_id","jank","ts","dur","jank_budget"
87654,34577,0,0,10000000,-31333333.350000
87654,34578,1,16000000,33000000,14666666.650000
87654,34579,0,55000000,33000000,-8333333.350000
"""))

  def test_touch_flow_event_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query="""
SELECT RUN_METRIC('chrome/touch_flow_event.sql');

SELECT
  trace_id,
  ts,
  dur,
  jank,
  step,
  ancestor_end,
  maybe_next_ancestor_ts,
  next_ts,
  next_trace_id,
  next_step
FROM touch_flow_event
ORDER BY touch_id, trace_id, ts;
""",
        out=Path('touch_flow_event_synth.out'))

  def test_touch_flow_event_queuing_delay_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query="""
SELECT RUN_METRIC('chrome/touch_flow_event_queuing_delay.sql');

SELECT
  trace_id,
  jank,
  step,
  next_step,
  ancestor_end,
  maybe_next_ancestor_ts,
  queuing_time_ns
FROM touch_flow_event_queuing_delay
ORDER BY trace_id, ts;
""",
        out=Path('touch_flow_event_queuing_delay_synth.out'))

  def test_memory_snapshot_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  (
    SELECT COUNT(*) FROM memory_snapshot
  ) AS total_snapshots,
  (
    SELECT COUNT(*) FROM process
  ) AS total_processes,
  (
    SELECT COUNT(*) FROM process_memory_snapshot
  ) AS total_process_snapshots,
  (
    SELECT COUNT(*) FROM memory_snapshot_node
  ) AS total_nodes,
  (
    SELECT COUNT(*) FROM memory_snapshot_edge
  ) AS total_edges,
  (
    SELECT COUNT(DISTINCT args.id)
    FROM args
    JOIN memory_snapshot_node
      ON args.arg_set_id = memory_snapshot_node.arg_set_id
  ) AS total_node_args,
  (
    SELECT COUNT(*) FROM profiler_smaps
    JOIN memory_snapshot ON timestamp = ts
  ) AS total_smaps;
""",
        out=Path('memory_snapshot_general_validation.out'))

  def test_memory_snapshot_os_dump_events(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  p.upid,
  pid,
  p.name,
  timestamp,
  detail_level,
  pf.value AS private_footprint_kb,
  prs.value AS peak_resident_set_kb,
  EXTRACT_ARG(p.arg_set_id, 'is_peak_rss_resettable') AS is_peak_rss_resettable
FROM process p
LEFT JOIN memory_snapshot
LEFT JOIN (
  SELECT id, upid
  FROM process_counter_track
  WHERE name = 'chrome.private_footprint_kb'
  ) AS pct_pf
  ON p.upid = pct_pf.upid
LEFT JOIN counter pf ON timestamp = pf.ts AND pct_pf.id = pf.track_id
LEFT JOIN (
  SELECT id, upid
  FROM process_counter_track
  WHERE name = 'chrome.peak_resident_set_kb'
  ) AS pct_prs
  ON p.upid = pct_prs.upid
LEFT JOIN counter prs ON timestamp = prs.ts AND pct_prs.id = prs.track_id
ORDER BY timestamp;
""",
        out=Path('memory_snapshot_os_dump_events.out'))

  def test_memory_snapshot_chrome_dump_events(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  pms.id AS process_snapshot_id,
  upid,
  snapshot_id,
  timestamp,
  detail_level
FROM memory_snapshot ms
LEFT JOIN process_memory_snapshot pms
  ON ms.id = pms.snapshot_id;
""",
        out=Path('memory_snapshot_chrome_dump_events.out'))

  def test_memory_snapshot_nodes(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  id,
  process_snapshot_id,
  parent_node_id,
  path,
  size,
  effective_size
FROM memory_snapshot_node
LIMIT 20;
""",
        out=Path('memory_snapshot_nodes.out'))

  def test_memory_snapshot_edges(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  id,
  source_node_id,
  target_node_id,
  importance
FROM memory_snapshot_edge
LIMIT 20;
""",
        out=Path('memory_snapshot_edges.out'))

  def test_memory_snapshot_node_args(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  node.id AS node_id,
  key,
  value_type,
  int_value,
  string_value
FROM memory_snapshot_node node
JOIN args ON node.arg_set_id = args.arg_set_id
LIMIT 20;
""",
        out=Path('memory_snapshot_node_args.out'))

  def test_memory_snapshot_smaps(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query="""
SELECT
  process.upid,
  process.name,
  smap.ts,
  path,
  size_kb,
  private_dirty_kb,
  swap_kb,
  file_name,
  start_address,
  module_timestamp,
  module_debugid,
  module_debug_path,
  protection_flags,
  private_clean_resident_kb,
  shared_dirty_resident_kb,
  shared_clean_resident_kb,
  locked_kb,
  proportional_resident_kb
FROM process
JOIN profiler_smaps smap ON process.upid = smap.upid
JOIN memory_snapshot ms ON ms.timestamp = smap.ts
LIMIT 20;
""",
        out=Path('memory_snapshot_smaps.out'))

  def test_combined_rail_modes(self):
    return DiffTestBlueprint(
        trace=Path('combined_rail_modes.py'),
        query="""
SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT * FROM combined_overall_rail_slices;
""",
        out=Csv("""
"id","ts","dur","rail_mode"
1,0,10000,"response"
2,10000,25000,"animation"
3,35000,10000,"background"
"""))

  def test_cpu_time_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('cpu_time_by_combined_rail_mode.py'),
        query="""
SELECT RUN_METRIC('chrome/cpu_time_by_rail_mode.sql');
SELECT * FROM cpu_time_by_rail_mode;
""",
        out=Csv("""
"id","ts","dur","rail_mode","cpu_dur"
1,0,10000,"response",26000
2,10000,20000,"animation",20000
3,30000,5000,"background",8000
4,35000,10000,"animation",21000
5,45000,10000,"background",1000
"""))

  def test_actual_power_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('actual_power_by_combined_rail_mode.py'),
        query="""
SELECT RUN_METRIC('chrome/actual_power_by_rail_mode.sql');
SELECT * FROM real_power_by_rail_mode;
""",
        out=Csv("""
"id","ts","dur","rail_mode","subsystem","joules","drain_w"
1,0,10000000,"response","cellular",0.000000,0.000000
1,0,10000000,"response","cpu_little",0.000140,0.014000
2,10000000,20000000,"animation","cellular",0.000350,0.017500
2,10000000,20000000,"animation","cpu_little",0.000140,0.007000
3,30000000,5000000,"background","cellular",0.000018,0.003500
3,30000000,5000000,"background","cpu_little",0.000007,0.001400
4,35000000,10000000,"animation","cellular",0.000021,0.002100
4,35000000,10000000,"animation","cpu_little",0.000070,0.007000
5,45000000,10000000,"background","cellular",0.000003,0.000350
5,45000000,10000000,"background","cpu_little",0.000070,0.007000
"""))

  def test_estimated_power_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('estimated_power_by_combined_rail_mode.py'),
        query="""
SELECT RUN_METRIC('chrome/estimated_power_by_rail_mode.sql');
SELECT * FROM power_by_rail_mode;
""",
        out=Csv("""
"id","ts","dur","rail_mode","mas","ma"
1,0,10000000,"response",0.554275,55.427500
2,10000000,20000000,"animation",0.284850,14.242500
3,30000000,5000000,"background",0.076233,15.246667
4,35000000,10000000,"animation",0.536850,53.685000
5,45000000,10000000,"background",0.071580,7.158000
"""))

  def test_modified_rail_modes(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes.py'),
        query="""
SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT * FROM modified_rail_slices;
""",
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,1950000000,"foreground_idle"
4,2950000000,333333324,"animation"
5,3283333324,216666676,"foreground_idle"
6,3500000000,1000000000,"background"
"""))

  def test_modified_rail_modes_no_vsyncs(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_no_vsyncs.py'),
        query="""
SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT * FROM modified_rail_slices;
""",
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,2500000000,"foreground_idle"
4,3500000000,1000000000,"background"
"""))

  def test_modified_rail_modes_with_input(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_with_input.py'),
        query="""
SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT * FROM modified_rail_slices;
""",
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,1950000000,"foreground_idle"
4,2950000000,50000000,"animation"
5,3000000000,66666674,"response"
6,3066666674,216666650,"animation"
7,3283333324,216666676,"foreground_idle"
8,3500000000,1000000000,"background"
"""))

  def test_modified_rail_modes_long(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_long.py'),
        query="""
SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT * FROM modified_rail_slices;
""",
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,1,"background"
"""))

  def test_modified_rail_modes_extra_long(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_extra_long.py'),
        query="""
SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT * FROM modified_rail_slices;
""",
        out=Csv("""
"id","ts","dur","mode"
"""))

  def test_chrome_processes(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT pid, name, process_type FROM chrome_process;
""",
        out=Csv("""
"pid","name","process_type"
18250,"Renderer","Renderer"
17547,"Browser","Browser"
18277,"GPU Process","Gpu"
17578,"Browser","Browser"
"""))

  def test_chrome_processes_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT pid, name, process_type FROM chrome_process;
""",
        out=Path('chrome_processes_android_systrace.out'))

  def test_chrome_threads(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT tid, name, is_main_thread, canonical_name
FROM chrome_thread
ORDER BY tid, name;
""",
        out=Path('chrome_threads.out'))

  def test_chrome_threads_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT tid, name, is_main_thread, canonical_name
FROM chrome_thread
ORDER BY tid, name;
""",
        out=Path('chrome_threads_android_systrace.out'))

  def test_chrome_processes_type(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT pid, name, string_value AS chrome_process_type
FROM
  process
JOIN
  (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
  ON
    process.arg_set_id = chrome_process_args.arg_set_id
ORDER BY pid;
""",
        out=Csv("""
"pid","name","chrome_process_type"
17547,"Browser","Browser"
17578,"Browser","Browser"
18250,"Renderer","Renderer"
18277,"GPU Process","Gpu"
"""))

  def test_chrome_processes_type_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query="""
SELECT pid, name, string_value AS chrome_process_type
FROM
  process
JOIN
  (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
  ON
    process.arg_set_id = chrome_process_args.arg_set_id
ORDER BY pid;
""",
        out=Path('chrome_processes_type_android_systrace.out'))

  def test_track_with_chrome_process(self):
    return DiffTestBlueprint(
        trace=Path('track_with_chrome_process.textproto'),
        query="""
SELECT pid, name, string_value AS chrome_process_type
FROM
  process
JOIN
  (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
  ON
    process.arg_set_id = chrome_process_args.arg_set_id
ORDER BY pid;
""",
        out=Csv("""
"pid","name","chrome_process_type"
5,"p5","[NULL]"
"""))

  def test_chrome_histogram_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_histogram_hashes.textproto'),
        query=Metric('chrome_histogram_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_histogram_hashes]: {
  hash: 10
  hash: 20
}
"""))

  def test_chrome_user_event_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_user_event_hashes.textproto'),
        query=Metric('chrome_user_event_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_user_event_hashes]: {
  action_hash: 10
  action_hash: 20
}

"""))

  def test_chrome_performance_mark_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_performance_mark_hashes.textproto'),
        query=Metric('chrome_performance_mark_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_performance_mark_hashes]: {
  site_hash: 10
  site_hash: 20
  mark_hash: 100
  mark_hash: 200
}
"""))

  def test_chrome_reliable_range(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
12,"First slice for utid=2","[NULL]",2
"""))

  def test_chrome_reliable_range_cropping(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_cropping.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
10000,"Range of interest packet","[NULL]",2
"""))

  def test_chrome_reliable_range_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_processes.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
1011,"Missing process data for upid=2",2,1
"""))

  def test_chrome_reliable_range_missing_browser_main(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_browser_main.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
1011,"Missing main thread for upid=1",1,1
"""))

  def test_chrome_reliable_range_missing_renderer_main(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_renderer_main.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
1011,"Missing main thread for upid=1",1,1
"""))

  def test_chrome_reliable_range_non_chrome_process(self):
    return DiffTestBlueprint(
        # We need a trace with a large number of non-chrome slices, so that the
        # reliable range is affected by their filtering.
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
  "start","reason","debug_limiting_upid","debug_limiting_utid"
  0,"[NULL]","[NULL]","[NULL]"
  """))

  def test_chrome_slice_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_slice_names.textproto'),
        query=Metric('chrome_slice_names'),
        out=TextProto(r"""
[perfetto.protos.chrome_slice_names]: {
  chrome_version_code: 123
  slice_name: "Looper.Dispatch: class1"
  slice_name: "name2"
}
"""))

  def test_chrome_tasks(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../data/chrome_page_load_all_categories_not_extended.pftrace.gz'
        ),
        query="""
SELECT RUN_METRIC('chrome/chrome_tasks.sql');

SELECT full_name, task_type, count() AS count
FROM chrome_tasks
GROUP BY full_name, task_type
ORDER BY count DESC
LIMIT 50;
""",
        out=Path('chrome_tasks.out'))

  def test_top_level_java_choreographer_slices_top_level_java_chrome_tasks_test(
      self):
    return DiffTestBlueprint(
        trace=Path('../../data/top_level_java_choreographer_slices'),
        query="""
SELECT RUN_METRIC(
  'chrome/chrome_tasks_template.sql',
  'slice_table_name', 'slice',
  'function_prefix', ''
);

SELECT
  full_name,
  task_type
FROM chrome_tasks
WHERE category = "toplevel,Java"
AND ts < 263904000000000
GROUP BY full_name, task_type;
""",
        out=Path(
            'top_level_java_choreographer_slices_top_level_java_chrome_tasks_test.out'
        ))

  def test_chrome_stack_samples_for_task_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_stack_traces_symbolized_trace.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_stack_samples_for_task.sql',
    'target_duration_ms', '0.000001',
    'thread_name', '"CrBrowserMain"',
    'task_name', '"sendTouchEvent"');

SELECT
  sample.description,
  sample.ts,
  sample.depth
FROM chrome_stack_samples_for_task sample
JOIN (
    SELECT
      ts,
      dur
    FROM slice
    WHERE ts = 696373965001470
) test_slice
ON sample.ts >= test_slice.ts
  AND sample.ts <= test_slice.ts + test_slice.dur
ORDER BY sample.ts, sample.depth;
""",
        out=Path('chrome_stack_samples_for_task_test.out'))

  def test_unsymbolized_args(self):
    return DiffTestBlueprint(
        trace=Path('unsymbolized_args.textproto'),
        query=Metric('chrome_unsymbolized_args'),
        out=TextProto(r"""
[perfetto.protos.chrome_unsymbolized_args]: {
  args {
     module: "/liblib.so"
     build_id: "6275696c642d6964"
     address: 123
     google_lookup_id: "6275696c642d6964"
   }
   args {
     module: "/libmonochrome_64.so"
     build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
     address: 234
     google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
   }
}"""))

  def test_async_trace_1_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-1.json'),
        query="""
SELECT COUNT(1) FROM slice;
""",
        out=Csv("""
"COUNT(1)"
16
"""))

  def test_async_trace_2_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-2.json'),
        query="""
SELECT COUNT(1) FROM slice;
""",
        out=Csv("""
"COUNT(1)"
35
"""))

  def test_chrome_args_class_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_args_class_names.textproto'),
        query=Metric('chrome_args_class_names'),
        out=TextProto(r"""

[perfetto.protos.chrome_args_class_names] {
  class_names_per_version {
    class_name: "abc"
    class_name: "def"
    class_name: "ghi"
    class_name: "jkl"
  }
}
"""))

  def test_chrome_log_message(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query="""
SELECT utid, tag, msg FROM android_logs;
""",
        out=Csv("""
"utid","tag","msg"
1,"foo.cc:123","log message"
"""))

  def test_chrome_log_message_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query=Path('chrome_log_message_args_test.sql'),
        out=Csv("""
"log_message","function_name","file_name","line_number"
"log message","func","foo.cc",123
"""))

  def test_chrome_missing_processes_default_trace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT upid, pid, reliable_from
FROM
  experimental_missing_chrome_processes
JOIN
  process
  USING(upid)
ORDER BY upid;
""",
        out=Csv("""
"upid","pid","reliable_from"
"""))

  def test_chrome_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query="""
SELECT upid, pid, reliable_from
FROM
  experimental_missing_chrome_processes
JOIN
  process
  USING(upid)
ORDER BY upid;
""",
        out=Csv("""
"upid","pid","reliable_from"
2,100,1000000000
3,1000,"[NULL]"
"""))

  def test_chrome_missing_processes_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query="""
SELECT arg_set_id, key, int_value
FROM
  slice
JOIN
  args
  USING(arg_set_id)
ORDER BY arg_set_id, key;
""",
        out=Csv("""
"arg_set_id","key","int_value"
2,"chrome_active_processes.pid[0]",10
2,"chrome_active_processes.pid[1]",100
2,"chrome_active_processes.pid[2]",1000
"""))

  def test_chrome_missing_processes_2(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query="""
SELECT upid, pid, reliable_from
FROM
  experimental_missing_chrome_processes
JOIN
  process
  USING(upid)
ORDER BY upid;
""",
        out=Csv("""
"upid","pid","reliable_from"
2,100,1000000000
3,1000,"[NULL]"
"""))

  def test_chrome_missing_processes_extension_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query="""
SELECT arg_set_id, key, int_value
FROM
  slice
JOIN
  args
  USING(arg_set_id)
ORDER BY arg_set_id, key;
""",
        out=Csv("""
"arg_set_id","key","int_value"
2,"active_processes.pid[0]",10
2,"active_processes.pid[1]",100
2,"active_processes.pid[2]",1000
"""))

  def test_chrome_custom_navigation_tasks(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_custom_navigation_trace.gz'),
        query="""
SELECT RUN_METRIC('chrome/chrome_tasks.sql');

SELECT full_name, task_type, count() AS count
FROM chrome_tasks
WHERE full_name GLOB 'FrameHost::BeginNavigation*'
  OR full_name GLOB 'FrameHost::DidCommitProvisionalLoad*'
  OR full_name GLOB 'FrameHost::DidCommitSameDocumentNavigation*'
  OR full_name GLOB 'FrameHost::DidStopLoading*'
GROUP BY full_name, task_type
ORDER BY count DESC
LIMIT 50;
""",
        out=Csv("""
"full_name","task_type","count"
"FrameHost::BeginNavigation (SUBFRAME)","navigation_task",5
"FrameHost::DidStopLoading (SUBFRAME)","navigation_task",3
"FrameHost::BeginNavigation (PRIMARY_MAIN_FRAME)","navigation_task",1
"FrameHost::DidCommitProvisionalLoad (SUBFRAME)","navigation_task",1
"""))

  def test_proto_content(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
SELECT path, SUM(total_size) as total_size
FROM experimental_proto_content as content 
JOIN experimental_proto_path as frame ON content.path_id = frame.id
GROUP BY path
ORDER BY total_size DESC, path
LIMIT 10;
""",
        out=Path('proto_content.out'))

  def test_chrome_scroll_jank_v2(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_scroll_jank_v2.sql');

SELECT
  scroll_processing_ms,
  scroll_jank_processing_ms,
  scroll_jank_percentage
FROM chrome_scroll_jank_v2_output;
""",
        out=Path('chrome_scroll_jank_v2.out'))
