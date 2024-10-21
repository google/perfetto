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


class Fuchsia(TestSuite):
  # Contains tests for parsing Fuchsia traces. Smoke test a bunch of different
  # types.
  def test_fuchsia_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace.fxt'),
        query="""
        SELECT
          ts,
          cpu,
          dur,
          end_state,
          priority,
          tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts
        LIMIT 10;
        """,
        out=Csv("""
        "ts","cpu","dur","end_state","priority","tid"
        19675868967,2,79022,"S",20,4344
        19676000188,3,504797,"S",20,6547
        19676504985,3,42877,"S",20,6525
        19676582005,0,48467,"S",20,11566
        19676989045,2,138116,"S",20,9949
        19677162311,3,48655,"S",20,6525
        19677305405,3,48814,"S",20,6525
        19677412330,0,177220,"S",20,4344
        19677680485,2,91422,"S",20,6537
        19677791779,3,96082,"S",20,1680
        """))

  def test_fuchsia_sched(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace_sched.fxt'),
        query="""
        SELECT
          ts,
          cpu,
          dur,
          end_state,
          priority,
          tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts
        LIMIT 10;
        """,
        out=Csv("""
        "ts","cpu","dur","end_state","priority","tid"
        68988611421,3,313611,"S",3122,3196
        68988925032,3,98697,"S",3122,23416
        68988957574,0,632536,"S",3122,3189
        68989023729,3,51371,"S",3122,3196
        68989075100,3,46773,"R",3122,25332
        68989121873,3,53620,"S",2147483647,24654
        68989175493,3,5241,"S",3122,25332
        68989180734,3,138507,"S",3122,30933
        68989319241,3,25028,"S",3122,30297
        68989344269,3,52723,"S",3122,28343
        """))

  def test_fuchsia_smoke_slices(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace.fxt'),
        query="""
        SELECT track.type AS type, depth, count(*) AS count
        FROM slice
        JOIN track ON slice.track_id = track.id
        GROUP BY track.type, depth
        ORDER BY track.type, depth;
        """,
        out=Csv("""
        "type","depth","count"
        "thread_track",0,2153
        "thread_track",1,1004
        """))

  def test_fuchsia_smoke_instants(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace.fxt'),
        query="""
        SELECT
          ts,
          name
        FROM slice
        WHERE
          dur = 0
        LIMIT 10;
        """,
        out=Csv("""
        "ts","name"
        21442756010,"task_start"
        21446583438,"task_end"
        21448366538,"task_start"
        21450363277,"task_end"
        21454255741,"task_start"
        21457834528,"task_end"
        21459006408,"task_start"
        21460601866,"task_end"
        21461282720,"task_start"
        21462998487,"task_end"
        """))

  def test_fuchsia_smoke_counters(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace.fxt'),
        query="""
        SELECT
          ts,
          value,
          name
        FROM counters
        LIMIT 10;
        """,
        out=Csv("""
        "ts","value","name"
        20329439768,30.331177,"cpu_usage:average_cpu_percentage:0"
        21331281870,7.829745,"cpu_usage:average_cpu_percentage:0"
        22332302017,9.669818,"cpu_usage:average_cpu_percentage:0"
        23332974162,6.421237,"cpu_usage:average_cpu_percentage:0"
        24333405767,12.079849,"cpu_usage:average_cpu_percentage:0"
        """))

  def test_fuchsia_smoke_flow(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace.fxt'),
        query="""
        SELECT
          id,
          slice_out,
          slice_in
        FROM flow
        LIMIT 10;
        """,
        out=Csv("""
        "id","slice_out","slice_in"
        0,0,1
        1,2,3
        2,4,5
        3,6,7
        4,8,9
        5,10,11
        6,12,13
        7,14,15
        8,16,17
        9,18,19
        """))

  def test_fuchsia_smoke_type(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_trace.fxt'),
        query="""
        SELECT
          id,
          name,
          type
        FROM track
        LIMIT 10;
        """,
        out=Csv("""
        "id","name","type"
        0,"[NULL]","thread_track"
        1,"[NULL]","thread_track"
        2,"[NULL]","thread_track"
        3,"[NULL]","thread_track"
        4,"[NULL]","thread_track"
        5,"cpu_usage:average_cpu_percentage:0","process_counter_track"
        6,"[NULL]","thread_track"
        7,"[NULL]","thread_track"
        8,"[NULL]","thread_track"
        9,"[NULL]","thread_track"
        """))

  # Smoke test a high-CPU trace.
  def test_fuchsia_workstation_smoke_slices(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_workstation.fxt'),
        query="""
        SELECT track.type AS type, depth, count(*) AS count
        FROM slice
        JOIN track ON slice.track_id = track.id
        GROUP BY track.type, depth
        ORDER BY track.type, depth;
        """,
        out=Path('fuchsia_workstation_smoke_slices.out'))

  def test_fuchsia_workstation_smoke_args(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_workstation.fxt'),
        query="""
        SELECT
          key,
          COUNT(*)
        FROM args
        GROUP BY key
        LIMIT 10;
        """,
        out=Csv("""
        "key","COUNT(*)"
        "Dart Arguments",3
        "Escher frame number",33
        "Expected presentation time",17
        "Frame number",33
        "MinikinFontsCount",2
        "Predicted frame duration(ms)",21
        "Render time(ms)",21
        "Timestamp",917
        "Update time(ms)",21
        "Vsync interval",900
        """))

  def test_fuchsia_args_import(self):
    return DiffTestBlueprint(
        trace=DataPath('fuchsia_events_and_args.fxt'),
        query="""
        SELECT key,int_value,string_value,real_value,value_type,display_value
        FROM args
        GROUP BY key
        ORDER BY key
        """,
        out=Csv("""
        "key","int_value","string_value","real_value","value_type","display_value"
        "SomeNullArg","[NULL]","null","[NULL]","string","null"
        "Somedouble","[NULL]","[NULL]",3.141500,"real","3.1415"
        "Someint32",-7,"[NULL]","[NULL]","int","-7"
        "Someint64",-234516543631231,"[NULL]","[NULL]","int","-234516543631231"
        "Someuint32",2145,"[NULL]","[NULL]","int","2145"
        "Someuint64",423621626134123415,"[NULL]","[NULL]","int","423621626134123415"
        "ping","[NULL]","pong","[NULL]","string","pong"
        "scope","[NULL]","[NULL]","[NULL]","string","[NULL]"
        "somebool",1,"[NULL]","[NULL]","bool","true"
        "somekoid",18,"[NULL]","[NULL]","int","18"
        "someotherbool",0,"[NULL]","[NULL]","bool","false"
        "someotherpointer",43981,"[NULL]","[NULL]","pointer","0xabcd"
        "somepointer",3285933758964,"[NULL]","[NULL]","pointer","0x2fd10ea19f4"
        "source","[NULL]","chrome","[NULL]","string","chrome"
        "source_scope","[NULL]","[NULL]","[NULL]","string","[NULL]"
        "trace_id",658,"[NULL]","[NULL]","int","658"
        "trace_id_is_process_scoped",0,"[NULL]","[NULL]","bool","false"
        "upid",1,"[NULL]","[NULL]","int","1"
        "utid",1,"[NULL]","[NULL]","int","1"
        """))
