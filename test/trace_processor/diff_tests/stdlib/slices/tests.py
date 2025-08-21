#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
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

  def test_thread_or_process_slice(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.with_context;

        SELECT name, ts, dur, depth, thread_name, tid, process_name, pid
        FROM thread_or_process_slice
        ORDER BY ts;
      """,
        out=Csv("""
        "name","ts","dur","depth","thread_name","tid","process_name","pid"
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

  def test_slice_remove_nulls_and_reparent(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE slices.hierarchy;

        SELECT id, parent_id, name, depth
        FROM _slice_remove_nulls_and_reparent!(
          (SELECT id, parent_id, depth, IIF(name = 'ProcessSlice', NULL, name) AS name
          FROM slice),
          name
        ) LIMIT 10;
      """,
        out=Csv("""
        "id","parent_id","name","depth"
        0,"[NULL]","AsyncSlice",0
        2,"[NULL]","ThreadSlice",0
        3,2,"NestedThreadSlice",0
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

  def test_thread_slice_time_in_state(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE slices.time_in_state;

        SELECT id, name, state, io_wait, blocked_function, dur
        FROM thread_slice_time_in_state
        LIMIT 10;
        """,
        out=Csv("""
          "id","name","state","io_wait","blocked_function","dur"
          0,"Deoptimization JIT inline cache","Running","[NULL]","[NULL]",178646
          1,"Deoptimization JIT inline cache","Running","[NULL]","[NULL]",119740
          2,"Lock contention on thread list lock (owner tid: 0)","Running","[NULL]","[NULL]",58073
          3,"Lock contention on thread list lock (owner tid: 0)","Running","[NULL]","[NULL]",98698
          3,"Lock contention on thread list lock (owner tid: 0)","S","[NULL]","[NULL]",56302
          4,"monitor contention with owner InputReader (1421) at void com.android.server.power.PowerManagerService.acquireWakeLockInternal(android.os.IBinder, int, java.lang.String, java.lang.String, android.os.WorkSource, java.lang.String, int, int)(PowerManagerService.java:1018) waiters=0 blocking from void com.android.server.power.PowerManagerService.handleSandman()(PowerManagerService.java:2280)","Running","[NULL]","[NULL]",121979
          4,"monitor contention with owner InputReader (1421) at void com.android.server.power.PowerManagerService.acquireWakeLockInternal(android.os.IBinder, int, java.lang.String, java.lang.String, android.os.WorkSource, java.lang.String, int, int)(PowerManagerService.java:1018) waiters=0 blocking from void com.android.server.power.PowerManagerService.handleSandman()(PowerManagerService.java:2280)","S","[NULL]","[NULL]",51198
          5,"monitor contention with owner main (1204) at void com.android.server.am.ActivityManagerService.onWakefulnessChanged(int)(ActivityManagerService.java:7244) waiters=0 blocking from void com.android.server.am.ActivityManagerService$3.handleMessage(android.os.Message)(ActivityManagerService.java:1704)","Running","[NULL]","[NULL]",45000
          5,"monitor contention with owner main (1204) at void com.android.server.am.ActivityManagerService.onWakefulnessChanged(int)(ActivityManagerService.java:7244) waiters=0 blocking from void com.android.server.am.ActivityManagerService$3.handleMessage(android.os.Message)(ActivityManagerService.java:1704)","S","[NULL]","[NULL]",20164377
          6,"monitor contention with owner main (1204) at void com.android.server.am.ActivityManagerService.onWakefulnessChanged(int)(ActivityManagerService.java:7244) waiters=1 blocking from com.android.server.wm.ActivityTaskManagerInternal$SleepToken com.android.server.am.ActivityTaskManagerService.acquireSleepToken(java.lang.String, int)(ActivityTaskManagerService.java:5048)","Running","[NULL]","[NULL]",35104
        """))

  def test_slice_self_dur(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|ParentSlice"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "B|10|ChildSlice"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2000
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2500
              pid: 10
              print {
                buf: "B|10|OtherSlice"
              }
            }
            event {
              timestamp: 3000
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.self_dur;

        SELECT
          s.name,
          sd.self_dur
        FROM slice_self_dur sd
        JOIN slice s ON s.id = sd.id
        ORDER BY s.name;
        """,
        out=Csv("""
        "name","self_dur"
        "ChildSlice",300
        "OtherSlice",500
        "ParentSlice",700
        """))
