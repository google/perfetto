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


class DiffTestModule_Track_event(DiffTestModule):

  def test_track_event_same_tids_threads(self):
    return DiffTestBlueprint(
        trace=Path('track_event_same_tids.textproto'),
        query=Path('../common/process_tracking_test.sql'),
        out=Csv("""
"tid","pid","pname","tname"
1,5,"[NULL]","t1"
1,10,"[NULL]","t2"
5,5,"[NULL]","[NULL]"
10,10,"[NULL]","[NULL]"
"""))

  def test_track_event_same_tids_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_same_tids.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Csv("""
"track","process","thread","thread_process","ts","dur","category","name"
"[NULL]","[NULL]","t1","[NULL]",1000,0,"cat","name1"
"[NULL]","[NULL]","t2","[NULL]",2000,0,"cat","name2"
"""))

  def test_track_event_typed_args_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_typed_args.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Csv("""
"track","process","thread","thread_process","ts","dur","category","name"
"[NULL]","[NULL]","t1","[NULL]",1000,0,"cat","name1"
"[NULL]","[NULL]","t1","[NULL]",2000,0,"cat","name2"
"[NULL]","[NULL]","t1","[NULL]",3000,0,"cat","name3"
"[NULL]","[NULL]","t1","[NULL]",4000,0,"cat","name4"
"[NULL]","[NULL]","t1","[NULL]",6000,0,"cat","name5"
"[NULL]","[NULL]","t1","[NULL]",7000,0,"cat","name6"
"""))

  def test_track_event_typed_args_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_typed_args.textproto'),
        query=Path('track_event_args_test.sql'),
        out=Path('track_event_typed_args_args.out'))

  def test_track_event_tracks_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_tracks_slices.out'))

  def test_track_event_tracks_processes(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query=Path('track_event_processes_test.sql'),
        out=Csv("""
"id","name","host_app"
0,"[NULL]","[NULL]"
1,"p1","host_app"
2,"p2","[NULL]"
"""))

  def test_track_event_tracks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query=Path('track_event_tracks_test.sql'),
        out=Csv("""
"name","parent_name","has_first_packet_on_sequence"
"Default Track","[NULL]","[NULL]"
"async","process=p1",1
"async2","process=p1",1
"async3","thread=t2",1
"event_and_track_async3","process=p1",1
"process=p1","[NULL]","[NULL]"
"process=p2","[NULL]","[NULL]"
"process=p2","[NULL]","[NULL]"
"thread=t1","process=p1",1
"thread=t2","process=p1",1
"thread=t3","process=p1",1
"thread=t4","process=p2","[NULL]"
"tid=1","[NULL]","[NULL]"
"""))

  def test_track_event_instant_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_instant.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Csv("""
"track","process","thread","thread_process","ts","dur","category","name"
"[NULL]","[NULL]","t1","[NULL]",1000,0,"cat","instant_on_t1"
"[NULL]","[NULL]","t1","[NULL]",2000,0,"cat","legacy_instant_on_t1"
"[NULL]","[NULL]","t1","[NULL]",3000,0,"cat","legacy_mark_on_t1"
"""))

  def test_legacy_async_event(self):
    return DiffTestBlueprint(
        trace=Path('legacy_async_event.textproto'),
        query=Path('track_event_slice_with_args_test.sql'),
        out=Path('legacy_async_event.out'))

  def test_track_event_with_atrace(self):
    return DiffTestBlueprint(
        trace=Path('track_event_with_atrace.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Csv("""
"track","process","thread","thread_process","ts","dur","category","name"
"[NULL]","[NULL]","t1","[NULL]",10000,1000,"cat","event1"
"[NULL]","[NULL]","t1","[NULL]",20000,8000,"cat","event2"
"[NULL]","[NULL]","t1","[NULL]",21000,7000,"[NULL]","atrace"
"""))

  def test_track_event_merged_debug_annotations_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_merged_debug_annotations.textproto'),
        query=Path('track_event_args_test.sql'),
        out=Path('track_event_merged_debug_annotations_args.out'))

  def test_track_event_counters_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Csv("""
"track","process","thread","thread_process","ts","dur","category","name"
"[NULL]","[NULL]","t1","Browser",1000,100,"cat","event1_on_t1"
"[NULL]","[NULL]","t1","Browser",2000,200,"cat","event2_on_t1"
"[NULL]","[NULL]","t1","Browser",2000,200,"cat","event3_on_t1"
"[NULL]","[NULL]","t1","Browser",4000,0,"cat","event4_on_t1"
"[NULL]","[NULL]","t4","Browser",4000,100,"cat","event1_on_t3"
"[NULL]","[NULL]","t1","Browser",4300,0,"cat","float_counter_on_t1"
"[NULL]","[NULL]","t1","Browser",4500,0,"cat","float_counter_on_t1"
"""))

  def test_track_event_counters_counters(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        query=Path('track_event_counters_test.sql'),
        out=Path('track_event_counters_counters.out'))

  def test_track_event_monotonic_trace_clock_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_monotonic_trace_clock.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Csv("""
"track","process","thread","thread_process","ts","dur","category","name"
"name1","[NULL]","[NULL]","[NULL]",1000,0,"cat","name1"
"name1","[NULL]","[NULL]","[NULL]",2000,0,"cat","name2"
"""))

  def test_track_event_chrome_histogram_sample_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_chrome_histogram_sample.textproto'),
        query=Path('track_event_args_test.sql'),
        out=Path('track_event_chrome_histogram_sample_args.out'))

  def test_flow_events_track_event(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_track_event.textproto'),
        query=Path('flow_events_test.sql'),
        out=Csv("""
"slice_out","slice_in"
"FlowSlice1Start","FlowSlice1End"
"FlowSlice1Start2Start","FlowSlice1End"
"FlowSlice1Start2Start","FlowSlice2End"
"FlowSlice3Begin","FlowSlice3End4Begin"
"FlowSlice3End4Begin","FlowSlice4Step"
"FlowSlice4Step","FlowSlice4Step2_FlowIdOnAsyncEndEvent"
"FlowSlice4Step2_FlowIdOnAsyncEndEvent","FlowSlice4End"
"""))

  def test_flow_events_proto_v2(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_proto_v2.textproto'),
        query=Path('flow_events_test.sql'),
        out=Csv("""
"slice_out","slice_in"
"FlowBeginSlice","FlowEndSlice_1"
"FlowBeginSlice","FlowStepSlice"
"FlowStepSlice","FlowEndSlice_2"
"""))

  def test_flow_events_proto_v1(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_proto_v1.textproto'),
        query=Path('flow_events_test.sql'),
        out=Csv("""
"slice_out","slice_in"
"FlowBeginSlice","FlowEndSlice_1"
"FlowEndSlice_1","FlowStepSlice"
"FlowStepSlice","FlowEndSlice_2"
"""))

  def test_experimental_slice_layout_depth(self):
    return DiffTestBlueprint(
        trace=Path('experimental_slice_layout_depth.py'),
        query=Path('experimental_slice_layout_depth_test.sql'),
        out=Csv("""
"layout_depth"
0
0
0
"""))

  def test_merging_regression(self):
    return DiffTestBlueprint(
        trace=Path('../../data/trace_with_descriptor.pftrace'),
        query=Path('merging_regression_test.sql'),
        out=Csv("""
"ts"
605361018360000
605361018360000
605361028265000
605361028265000
605361028361000
605361028878000
605361033445000
605361033445000
605361034257000
605361035040000
"""))

  def test_range_of_interest(self):
    return DiffTestBlueprint(
        trace=Path('range_of_interest.textproto'),
        query=Path('range_of_interest_test.sql'),
        out=Csv("""
"ts","name"
12000,"slice3"
13000,"slice4"
"""))
