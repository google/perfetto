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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ChromeMetrics(TestSuite):
  # Tests related to Chrome's use of Perfetto. Chrome histogram hashes
  def test_chrome_histogram_hashes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_event {
            categories: "cat1"
            type: 3
            name_iid: 1
            chrome_histogram_sample {
              name_hash: 10
              sample: 100
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_event {
            categories: "cat2"
            type: 3
            name_iid: 2
            chrome_histogram_sample {
              name_hash: 20
            }
          }
        }
        """),
        query=Metric('chrome_histogram_hashes'),
        out=TextProto(r"""
        [perfetto.protos.chrome_histogram_hashes]: {
          hash: 10
          hash: 20
        }
        """))

  # Chrome user events
  def test_chrome_user_event_hashes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_event {
            categories: "cat1"
            type: 3
            name_iid: 1
            chrome_user_event {
              action_hash: 10
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_event {
            categories: "cat2"
            type: 3
            name_iid: 2
            chrome_user_event {
              action_hash: 20
            }
          }
        }
        """),
        query=Metric('chrome_user_event_hashes'),
        out=TextProto(r"""
        [perfetto.protos.chrome_user_event_hashes]: {
          action_hash: 10
          action_hash: 20
        }
        """))

  # Chrome performance mark
  def test_chrome_performance_mark_hashes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_event {
            categories: "cat1"
            type: 3
            name: "name1"
            [perfetto.protos.ChromeTrackEvent.chrome_hashed_performance_mark] {
              site_hash: 10
              mark_hash: 100
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_event {
            categories: "cat2"
            type: 3
            name: "name2"
            [perfetto.protos.ChromeTrackEvent.chrome_hashed_performance_mark] {
              site_hash: 20
              mark_hash: 200
            }
          }
        }
        """),
        query=Metric('chrome_performance_mark_hashes'),
        out=TextProto(r"""
        [perfetto.protos.chrome_performance_mark_hashes]: {
          site_hash: 10
          site_hash: 20
          mark_hash: 100
          mark_hash: 200
        }
        """))

  # Chrome reliable range
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

  def test_chrome_reliable_range_missing_gpu_main(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_gpu_main.textproto'),
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
        trace=DataPath('example_android_trace_30s.pb'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
        "start","reason","debug_limiting_upid","debug_limiting_utid"
        0,"[NULL]","[NULL]","[NULL]"
        """))

  # Chrome slices
  def test_chrome_slice_names(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            categories: "cat"
            name: "Looper.Dispatch: class1"
            type: 3
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            categories: "cat"
            name: "name2"
            type: 3
          }
        }
        packet {
          chrome_metadata {
            chrome_version_code: 123
          }
        }
        """),
        query=Metric('chrome_slice_names'),
        out=TextProto(r"""
        [perfetto.protos.chrome_slice_names]: {
          chrome_version_code: 123
          slice_name: "Looper.Dispatch: class1"
          slice_name: "name2"
        }
        """))

  # Chrome stack samples.
  def test_chrome_stack_samples_for_task(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_stack_traces_symbolized_trace.pftrace'),
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

  # Trace proto content
  def test_proto_content(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_scroll_without_vsync.pftrace'),
        query=Path('proto_content_test.sql'),
        out=Path('proto_content.out'))

  # TODO(mayzner): Uncomment when it works
  # def test_proto_content_path(self):
  #   return DiffTestBlueprint(
  #       trace=DataPath('chrome_scroll_without_vsync.pftrace'),
  #       query=Path('proto_content_path_test.sql'),
  #       out=Csv("""
  #       "total_size","field_type","field_name","parent_id","event_category","event_name"
  #       137426,"TracePacket","[NULL]","[NULL]","[NULL]","[NULL]"
  #       59475,"TrackEvent","#track_event",415,"[NULL]","[NULL]"
  #       37903,"TrackEvent","#track_event",17,"[NULL]","[NULL]"
  #       35904,"int32","#trusted_uid",17,"[NULL]","[NULL]"
  #       35705,"TracePacket","[NULL]","[NULL]","input,benchmark","LatencyInfo.Flow"
  #       29403,"TracePacket","[NULL]","[NULL]","cc,input","[NULL]"
  #       24703,"ChromeLatencyInfo","#chrome_latency_info",18,"[NULL]","[NULL]"
  #       22620,"uint64","#time_us",26,"[NULL]","[NULL]"
  #       18711,"TrackEvent","#track_event",1467,"[NULL]","[NULL]"
  #       15606,"uint64","#timestamp",17,"[NULL]","[NULL]"
  #       """))
