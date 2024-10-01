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
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from google.protobuf import text_format


class Slices(TestSuite):

  def test_thread_slice(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.with_context;

        SELECT name, ts, dur, depth, thread_name, tid, process_name, pid
        FROM thread_slice;
      """,
        out=Csv("""
        "name","ts","dur","depth","thread_name","tid","process_name","pid"
        "ThreadSlice",5,8,0,"Thread",5,"Process",3
        "NestedThreadSlice",6,1,1,"Thread",5,"Process",3
      """))

  def test_process_slice(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.with_context;

        SELECT name, ts, dur, depth, process_name, pid
        FROM process_slice;
      """,
        out=Csv("""
        "name","ts","dur","depth","process_name","pid"
        "ProcessSlice",3,4,0,"Process",3
      """))

  def test_slice_with_process_and_thread_info(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.slices;

        SELECT name, ts, dur, depth, thread_name, tid, process_name, pid
        FROM _slice_with_thread_and_process_info;
      """,
        out=Csv("""
        "name","ts","dur","depth","thread_name","tid","process_name","pid"
        "AsyncSlice",1,2,0,"[NULL]","[NULL]","[NULL]","[NULL]"
        "ProcessSlice",3,4,0,"[NULL]","[NULL]","Process",3
        "ThreadSlice",5,8,0,"Thread",5,"Process",3
        "NestedThreadSlice",6,1,1,"Thread",5,"Process",3
      """))

  # Ancestor / descendant wrappers.

  def test_slice_ancestor_and_self(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.hierarchy;

        SELECT name, ts, dur, depth
        FROM _slice_ancestor_and_self(
          (SELECT id FROM slice WHERE name = 'NestedThreadSlice')
        );
      """,
        out=Csv("""
        "name","ts","dur","depth"
        "NestedThreadSlice",6,1,1
        "ThreadSlice",5,8,0
      """))

  def test_slice_descendant_and_self(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.hierarchy;

        SELECT name, ts, dur, depth
        FROM _slice_descendant_and_self(
          (SELECT id FROM slice WHERE name = 'ThreadSlice')
        );
      """,
        out=Csv("""
        "name","ts","dur","depth"
        "ThreadSlice",5,8,0
        "NestedThreadSlice",6,1,1
      """))

  # Common functions

  def test_slice_flattened(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE slices.flat_slices;

        SELECT e.name, e.ts, e.dur, e.depth
        FROM _slice_flattened e
          JOIN thread_track ON e.track_id = thread_track.id
          JOIN thread USING(utid)
        WHERE thread.tid = 30196
        ORDER BY ts
        LIMIT 10;
      """,
        out=Csv("""
        "name","ts","dur","depth"
        "EventForwarder::OnTouchEvent",1035865509936036,211000,0
        "GestureProvider::OnTouchEvent",1035865510147036,87000,1
        "EventForwarder::OnTouchEvent",1035865510234036,48000,0
        "RenderWidgetHostImpl::ForwardTouchEvent",1035865510282036,41000,1
        "LatencyInfo.Flow",1035865510323036,8000,2
        "RenderWidgetHostImpl::ForwardTouchEvent",1035865510331036,16000,1
        "PassthroughTouchEventQueue::QueueEvent",1035865510347036,30000,2
        "InputRouterImpl::FilterAndSendWebInputEvent",1035865510377036,8000,3
        "LatencyInfo.Flow",1035865510385036,126000,4
        "RenderWidgetHostImpl::UserInputStarted",1035865510511036,7000,5
      """))

  def test_thread_slice_cpu_time(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE slices.cpu_time;

        SELECT id, cpu_time
        FROM thread_slice_cpu_time
        LIMIT 10;
        """,
        out=Csv("""
        "id","cpu_time"
        0,178646
        1,119740
        2,58073
        3,98698
        4,121979
        5,45000
        6,35104
        7,33333
        8,46926
        9,17865
        """))