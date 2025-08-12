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

from python.generators.diff_tests.testing import Path, DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class StdlibSched(TestSuite):

  def test_runnable_thread_count(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
      SELECT * FROM sched_runnable_thread_count;
      """,
        out=Csv("""
      "ts","runnable_thread_count"
      1,1
      50,1
      100,2
      115,2
      120,2
      170,3
      250,2
      390,2
      """))

  def test_active_cpu_count(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;

      SELECT * FROM sched_active_cpu_count;
      """,
        out=Csv("""
      "ts","active_cpu_count"
      1,1
      50,2
      100,2
      115,2
      120,2
      170,1
      250,2
      390,2
      """))

  def test_sched_time_in_state_for_thread(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_time_in_state_for_thread
        ORDER BY utid, state
        LIMIT 10;
        """,
        out=Csv("""
        "utid","total_runtime","state","time_in_state","percentage_in_state"
        1,27540674878,"D",596720,0
        1,27540674878,"R",1988438,0
        1,27540674878,"R+",2435415,0
        1,27540674878,"Running",23098223,0
        1,27540674878,"S",27512556082,99
        2,27761417087,"D",833039830,3
        2,27761417087,"R+",2931096,0
        2,27761417087,"Running",92350845,0
        2,27761417087,"S",26833095316,96
        3,29374171050,"R",140800325,0
        """))

  def test_sched_percentage_of_time_in_state(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_percentage_of_time_in_state
        ORDER BY utid
        LIMIT 10;
        """,
        out=Csv("""
        "utid","running","runnable","runnable_preempted","sleeping","uninterruptible_sleep","other"
        1,0,0,0,99,0,"[NULL]"
        2,0,"[NULL]",0,96,3,"[NULL]"
        3,5,0,0,93,"[NULL]","[NULL]"
        4,100,"[NULL]","[NULL]","[NULL]","[NULL]",0
        5,0,0,0,99,0,"[NULL]"
        6,0,"[NULL]",0,99,"[NULL]","[NULL]"
        7,0,0,0,99,"[NULL]","[NULL]"
        8,0,0,0,98,0,"[NULL]"
        9,0,"[NULL]","[NULL]",99,"[NULL]","[NULL]"
        10,0,"[NULL]",0,99,"[NULL]","[NULL]"
        """))

  def test_sched_time_in_state_for_thread_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_time_in_state_for_thread_in_interval(71039311397, 10000000000, 44);
        """,
        out=Csv("""
        "state","io_wait","blocked_function","dur"
        "S","[NULL]","[NULL]",9994400675
        "Running","[NULL]","[NULL]",4655524
        "D","[NULL]","[NULL]",563645
        "R+","[NULL]","[NULL]",380156
        """))

  def test_sched_time_in_state_and_cpu_for_thread_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_time_in_state_and_cpu_for_thread_in_interval(71039311397, 10000000000, 44);
        """,
        out=Csv("""
        "state","io_wait","cpu","blocked_function","dur"
        "S","[NULL]","[NULL]","[NULL]",9994400675
        "Running","[NULL]",2,"[NULL]",4655524
        "D","[NULL]","[NULL]","[NULL]",563645
        "R+","[NULL]","[NULL]","[NULL]",380156
        """))

  def test_sched_time_in_state_for_cpu_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
      INCLUDE PERFETTO MODULE sched.time_in_state;
      SELECT * FROM
      sched_time_in_state_for_cpu_in_interval(1, TRACE_START(), TRACE_DUR());
      """,
        out=Csv("""
        "end_state","dur"
        "D",311982601
        "DK",31103960
        "R",23230879715
        "R+",1148673560
        "S",3868233011
        "x",35240577
      """))

  def test_sched_previous_runnable_on_thread(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE sched.runnable;

        SELECT
          running.ts AS running_ts,
          running_thread.tid AS running_tid,
          prev_runnable.ts AS prev_runnable_ts,
          prev_runnable_thread.tid AS prev_runnable_tid,
          prev_wakeup.ts AS prev_wakeup_ts,
          prev_wakeup_thread.tid AS prev_wakeup_tid
        FROM sched_previous_runnable_on_thread s
        JOIN thread_state running ON running.id = s.id
        JOIN thread running_thread ON running_thread.utid = running.utid
        JOIN thread_state prev_runnable ON prev_runnable.id = s.prev_runnable_id
        JOIN thread prev_runnable_thread ON prev_runnable_thread.utid = prev_runnable.utid
        JOIN thread_state prev_wakeup ON prev_wakeup.id = s.prev_wakeup_runnable_id
        JOIN thread prev_wakeup_thread ON prev_wakeup_thread.utid = prev_wakeup.utid
        ORDER BY running.ts DESC, running_thread.tid DESC
        LIMIT 10;
        """,
        out=Csv("""
        "running_ts","running_tid","prev_runnable_ts","prev_runnable_tid","prev_wakeup_ts","prev_wakeup_tid"
        9610742069,509,9610595870,509,9610595870,509
        9610725508,509,9610595870,509,9610595870,509
        9610687789,509,9610595870,509,9610595870,509
        9610565596,2246,9609128381,2246,9609128381,2246
        9610462325,509,9608319340,509,9578107751,509
        9610366255,889,9610234867,889,9532969161,889
        9610258305,1469,9610253422,1469,9603595910,1469
        9610253422,893,9533011926,893,9533011926,893
        9610234867,1469,9603595910,1469,9603595910,1469
        9610202640,889,9610190108,889,9532969161,889
        """))

  def test_sched_latency(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE sched.latency;

        SELECT
          running.ts,
          thread.tid,
          latency.latency_dur,
          sched.cpu,
          sched.dur
        FROM sched_latency_for_running_interval latency
        JOIN thread_state running ON running.id = latency.thread_state_id
        JOIN thread ON thread.utid = latency.utid
        JOIN sched ON sched.id = latency.sched_id AND sched.utid = latency.utid
        ORDER BY running.ts DESC, thread.tid DESC
        LIMIT 10;
        """,
        out=Csv("""
        "ts","tid","latency_dur","cpu","dur"
        9610742069,509,91919,7,6999
        9610725508,509,91919,7,6633
        9610687789,509,91919,7,20507
        9610565596,2246,1437215,7,122193
        9610462325,509,826823,7,18229
        9610366255,889,131388,7,96070
        9610258305,1469,4883,7,107950
        9610253422,893,469849,7,4883
        9610234867,1469,670736,7,18555
        9610202640,889,12532,7,32227
        """))
