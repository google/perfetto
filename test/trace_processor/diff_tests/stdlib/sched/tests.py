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

  def test_sched_utilization_per_second(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.utilization.system;

        SELECT * FROM sched_utilization_per_second;
        """,
        out=Csv("""
        "ts","utilization","unnormalized_utilization"
        70000000000,0.004545,0.036362
        71000000000,0.022596,0.180764
        72000000000,0.163393,1.307146
        73000000000,0.452122,3.616972
        74000000000,0.525557,4.204453
        75000000000,0.388632,3.109057
        76000000000,0.425447,3.403579
        77000000000,0.201112,1.608896
        78000000000,0.280247,2.241977
        79000000000,0.345228,2.761827
        80000000000,0.303258,2.426064
        81000000000,0.487522,3.900172
        82000000000,0.080542,0.644336
        83000000000,0.362450,2.899601
        84000000000,0.076438,0.611501
        85000000000,0.110689,0.885514
        86000000000,0.681488,5.451901
        87000000000,0.808331,6.466652
        88000000000,0.941768,7.534142
        89000000000,0.480556,3.844446
        90000000000,0.453268,3.626142
        91000000000,0.280310,2.242478
        92000000000,0.006381,0.051049
        93000000000,0.030991,0.247932
        94000000000,0.031981,0.255845
        95000000000,0.027931,0.223446
        96000000000,0.063066,0.504529
        97000000000,0.023847,0.190773
        98000000000,0.011291,0.090328
        99000000000,0.024065,0.192518
        100000000000,0.001964,0.015711
        """))

  def test_sched_process_utilization_per_second(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.utilization.process;

        SELECT *
        FROM sched_process_utilization_per_second(10);
        """,
        out=Csv("""
        "ts","utilization","unnormalized_utilization"
        72000000000,0.000187,0.001495
        73000000000,0.000182,0.001460
        77000000000,0.000072,0.000579
        78000000000,0.000275,0.002204
        82000000000,0.000300,0.002404
        83000000000,0.000004,0.000034
        87000000000,0.000133,0.001065
        88000000000,0.000052,0.000416
        89000000000,0.000212,0.001697
        92000000000,0.000207,0.001658
        97000000000,0.000353,0.002823
        """))

  def test_sched_thread_utilization_per_second(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.utilization.thread;

        SELECT *
        FROM sched_thread_utilization_per_second(10);
        """,
        out=Csv("""
        "ts","utilization","unnormalized_utilization"
        70000000000,0.000024,0.000195
        72000000000,0.000025,0.000200
        73000000000,0.000053,0.000420
        74000000000,0.000044,0.000352
        75000000000,0.000058,0.000461
        76000000000,0.000075,0.000603
        77000000000,0.000051,0.000407
        78000000000,0.000047,0.000374
        79000000000,0.000049,0.000396
        80000000000,0.000084,0.000673
        81000000000,0.000041,0.000329
        82000000000,0.000048,0.000383
        83000000000,0.000040,0.000323
        84000000000,0.000018,0.000145
        85000000000,0.000053,0.000421
        86000000000,0.000121,0.000972
        87000000000,0.000049,0.000392
        88000000000,0.000036,0.000285
        89000000000,0.000033,0.000266
        90000000000,0.000050,0.000401
        91000000000,0.000025,0.000201
        92000000000,0.000009,0.000071
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

        SELECT *
        FROM sched_previous_runnable_on_thread
        WHERE prev_wakeup_runnable_id IS NOT NULL
        ORDER BY id DESC
        LIMIT 10;
        """,
        out=Csv("""
        "id","prev_runnable_id","prev_wakeup_runnable_id"
        538199,538191,538191
        538197,538191,538191
        538195,538191,538191
        538190,538136,538136
        538188,538088,533235
        538184,538176,524613
        538181,538178,537492
        538179,524619,524619
        538177,537492,537492
        538175,538174,524613
        """))
