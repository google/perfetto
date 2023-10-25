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
from python.generators.diff_tests.testing import PrintProfileProto


class TablesSched(TestSuite):
  # Sched table
  def test_synth_1_filter_sched(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query="""
        SELECT ts, cpu, dur FROM sched
        WHERE
          cpu = 1
          AND dur > 50
          AND dur <= 100
          AND ts >= 100
          AND ts <= 400;
        """,
        out=Csv("""
        "ts","cpu","dur"
        170,1,80
        """))

  def test_android_sched_and_ps_b119496959(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT ts, cpu FROM sched WHERE ts >= 81473797418963 LIMIT 10;
        """,
        out=Csv("""
        "ts","cpu"
        81473797824982,3
        81473797942847,3
        81473798135399,0
        81473798786857,2
        81473798875451,3
        81473799019930,2
        81473799079982,0
        81473800089357,3
        81473800144461,3
        81473800441805,3
        """))

  def test_android_sched_and_ps_b119301023(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT ts FROM sched
        WHERE ts > 0.1 + 1e9
        LIMIT 10;
        """,
        out=Csv("""
        "ts"
        81473010031230
        81473010109251
        81473010121751
        81473010179772
        81473010203886
        81473010234720
        81473010278522
        81473010308470
        81473010341386
        81473010352792
        """))

  def test_sched_wakeup(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        SELECT * FROM spurious_sched_wakeup
        ORDER BY ts LIMIT 10
        """,
        out=Csv("""
        "id","type","ts","thread_state_id","irq_context","utid","waker_utid"
        0,"spurious_sched_wakeup",1735850782904,423,0,230,1465
        1,"spurious_sched_wakeup",1736413914899,886,0,230,1467
        2,"spurious_sched_wakeup",1736977755745,1298,0,230,1469
        3,"spurious_sched_wakeup",1737046900004,1473,0,1472,1473
        4,"spurious_sched_wakeup",1737047159060,1502,0,1474,1472
        5,"spurious_sched_wakeup",1737081636170,2992,0,1214,1319
        6,"spurious_sched_wakeup",1737108696536,5010,0,501,557
        7,"spurious_sched_wakeup",1737153309978,6431,0,11,506
        8,"spurious_sched_wakeup",1737165240546,6915,0,565,499
        9,"spurious_sched_wakeup",1737211563344,8999,0,178,1195
        """))

  def test_raw_common_flags(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        SELECT * FROM raw WHERE common_flags != 0 ORDER BY ts LIMIT 10
        """,
        out=Csv("""
        "id","type","ts","name","cpu","utid","arg_set_id","common_flags"
        3,"ftrace_event",1735489788930,"sched_waking",0,300,4,1
        4,"ftrace_event",1735489812571,"sched_waking",0,300,5,1
        5,"ftrace_event",1735489833977,"sched_waking",1,305,6,1
        8,"ftrace_event",1735489876788,"sched_waking",1,297,9,1
        9,"ftrace_event",1735489879097,"sched_waking",0,304,10,1
        12,"ftrace_event",1735489933912,"sched_waking",0,428,13,1
        14,"ftrace_event",1735489972385,"sched_waking",1,232,15,1
        17,"ftrace_event",1735489999987,"sched_waking",1,232,15,1
        19,"ftrace_event",1735490039439,"sched_waking",1,298,18,1
        20,"ftrace_event",1735490042084,"sched_waking",1,298,19,1
        """))

  def test_thread_executing_span_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT
          root_id,
          parent_id,
          id,
          ts,
          dur,
          utid,
          waker_utid,
          blocked_dur,
          blocked_state,
          blocked_function,
          is_root,
          depth
        FROM experimental_thread_executing_span_graph
          WHERE blocked_function IS NOT NULL
        ORDER BY ts
        LIMIT 10
        """,
        out=Csv("""
        "root_id","parent_id","id","ts","dur","utid","waker_utid","blocked_dur","blocked_state","blocked_function","is_root","depth"
        25,377,380,1735842234188,283571,46,427,351402620,"I","worker_thread",0,8
        25,402,405,1735843726296,8545303,46,427,1208537,"I","worker_thread",0,6
        25,419,432,1735850643698,16245,95,1465,154087,"I","worker_thread",0,7
        25,443,446,1735851953029,554638012,95,427,1103252,"I","worker_thread",0,9
        25,500,503,1735886367018,191863,46,427,34095419,"I","worker_thread",0,13
        25,446,667,1736125372478,52493,46,95,238813597,"I","worker_thread",0,10
        25,835,838,1736405409972,278036,46,427,279985001,"I","worker_thread",0,15
        25,862,865,1736406817672,7959441,46,427,1129664,"I","worker_thread",0,13
        25,882,889,1736413734042,25870,95,1467,7143001,"I","worker_thread",0,14
        25,882,894,1736413763072,31692550,11,1467,4413060,"I","rcu_gp_fqs_loop",0,14
        """))

  def test_thread_executing_span_graph_contains_forked_states(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT
          root_id,
          parent_id,
          id,
          ts,
          dur,
          utid,
          waker_utid,
          blocked_dur,
          blocked_state,
          blocked_function,
          is_root,
          depth
        FROM experimental_thread_executing_span_graph
          WHERE ts = 1735842081507 AND dur = 293868
        """,
        out=Csv("""
        "root_id","parent_id","id","ts","dur","utid","waker_utid","blocked_dur","blocked_state","blocked_function","is_root","depth"
        25,369,376,1735842081507,293868,1465,230,"[NULL]","[NULL]","[NULL]",0,7
        """))

  def test_thread_executing_span_internal_runnable_state_has_no_running(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT COUNT(*) AS count FROM internal_runnable_state WHERE state = 'Running'
        """,
        out=Csv("""
        "count"
        0
        """))

  def test_thread_executing_span_graph_has_no_null_dur(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT ts,dur FROM experimental_thread_executing_span_graph
          WHERE dur IS NULL OR ts IS NULL
        """,
        out=Csv("""
        "ts","dur"
        """))

  def test_thread_executing_span_graph_accepts_null_irq_context(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT COUNT(*) AS count FROM experimental_thread_executing_span_graph
        """,
        out=Csv("""
        "count"
        25
        """))

  def test_thread_executing_span_critical_path_all(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT
          id,
          ts,
          dur,
          utid,
          critical_path_id,
          critical_path_blocked_dur,
          critical_path_blocked_state,
          critical_path_blocked_function,
          critical_path_utid INT
        FROM experimental_thread_executing_span_critical_path(NULL, start_ts, end_ts), trace_bounds
        ORDER BY ts
        LIMIT 10
        """,
        out=Csv("""
        "id","ts","dur","utid","critical_path_id","critical_path_blocked_dur","critical_path_blocked_state","critical_path_blocked_function","INT"
        5,1735489812571,83938,304,5,"[NULL]","[NULL]","[NULL]",304
        6,1735489833977,52463,297,6,"[NULL]","[NULL]","[NULL]",297
        11,1735489876788,76985,428,11,"[NULL]","[NULL]","[NULL]",428
        12,1735489879097,338180,243,12,"[NULL]","[NULL]","[NULL]",243
        17,1735489933912,653746,230,17,"[NULL]","[NULL]","[NULL]",230
        25,1735489999987,55979,298,25,4178,"S","[NULL]",298
        25,1735489999987,45838,298,1567,1561612705,"S","[NULL]",300
        25,1735489999987,45838,298,2014,1572044057,"S","[NULL]",305
        25,1735489999987,45838,298,2021,1572057416,"S","[NULL]",297
        28,1735490039439,570799,421,28,"[NULL]","[NULL]","[NULL]",421
        """))

  def test_thread_executing_span_critical_path_utid(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT
          id,
          ts,
          dur,
          utid,
          critical_path_id,
          critical_path_blocked_dur,
          critical_path_blocked_state,
          critical_path_blocked_function,
          critical_path_utid INT
        FROM experimental_thread_executing_span_critical_path((select utid from thread where tid = 3487), start_ts, end_ts), trace_bounds
        ORDER BY ts
        LIMIT 10
        """,
        out=Csv("""
        "id","ts","dur","utid","critical_path_id","critical_path_blocked_dur","critical_path_blocked_state","critical_path_blocked_function","INT"
        11889,1737349401439,7705561,1477,11889,"[NULL]","[NULL]","[NULL]",1477
        11952,1737357107000,547583,1480,11980,547583,"S","[NULL]",1477
        11980,1737357654583,8430762,1477,11980,547583,"S","[NULL]",1477
        12052,1737366085345,50400,91,12057,50400,"S","[NULL]",1477
        12057,1737366135745,6635927,1477,12057,50400,"S","[NULL]",1477
        12081,1737372771672,12798314,1488,12254,12798314,"S","[NULL]",1477
        12254,1737385569986,21830622,1477,12254,12798314,"S","[NULL]",1477
        12517,1737407400608,241267,91,12521,241267,"S","[NULL]",1477
        12521,1737407641875,1830015,1477,12521,241267,"S","[NULL]",1477
        12669,1737409471890,68590,91,12672,68590,"S","[NULL]",1477
        """))

  def test_thread_executing_span_critical_path_stack(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT
          id,
          ts,
          dur,
          utid,
          stack_depth,
          name,
          table_name,
          critical_path_utid
        FROM experimental_thread_executing_span_critical_path_stack((select utid from thread where tid = 3487), start_ts, end_ts), trace_bounds
        ORDER BY ts
        LIMIT 11
        """,
        out=Csv("""
        "id","ts","dur","utid","stack_depth","name","table_name","critical_path_utid"
        11889,1737349401439,57188,1477,0,"thread_state: R","thread_state",1477
        11889,1737349401439,57188,1477,1,"[NULL]","thread_state",1477
        11889,1737349401439,57188,1477,2,"[NULL]","thread_state",1477
        11889,1737349401439,57188,1477,3,"process_name: com.android.providers.media.module","thread_state",1477
        11889,1737349401439,57188,1477,4,"thread_name: rs.media.module","thread_state",1477
        11891,1737349458627,1884896,1477,0,"thread_state: Running","thread_state",1477
        11891,1737349458627,1884896,1477,1,"[NULL]","thread_state",1477
        11891,1737349458627,1884896,1477,2,"[NULL]","thread_state",1477
        11891,1737349458627,1884896,1477,3,"process_name: com.android.providers.media.module","thread_state",1477
        11891,1737349458627,1884896,1477,4,"thread_name: rs.media.module","thread_state",1477
        11891,1737349458627,1884896,1477,5,"cpu: 0","thread_state",1477
        """))

  def test_thread_executing_span_critical_path_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE experimental.thread_executing_span;
        SELECT HEX(pprof) FROM experimental_thread_executing_span_critical_path_graph("critical path", (select utid from thread where tid = 3487), 1737488133487, 16000), trace_bounds
      """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
        Sample:
        Values: 0
        Stack:
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: R (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        binder reply (0x0)
        blocking thread_name: binder:553_3 (0x0)
        blocking process_name: /system/bin/mediaserver (0x0)
        blocking thread_state: Running (0x0)
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        blocking process_name: /system/bin/mediaserver (0x0)
        blocking thread_state: Running (0x0)
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        blocking thread_name: binder:553_3 (0x0)
        blocking process_name: /system/bin/mediaserver (0x0)
        blocking thread_state: Running (0x0)
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        blocking thread_state: Running (0x0)
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        process_name: com.android.providers.media.module (0x0)
        thread_state: R (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: R (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        thread_state: R (0x0)
        critical path (0x0)

        Sample:
        Values: 0
        Stack:
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 1101
        Stack:
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: R (0x0)
        critical path (0x0)

        Sample:
        Values: 13010
        Stack:
        cpu: 0 (0x0)
        binder reply (0x0)
        blocking thread_name: binder:553_3 (0x0)
        blocking process_name: /system/bin/mediaserver (0x0)
        blocking thread_state: Running (0x0)
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)

        Sample:
        Values: 1889
        Stack:
        cpu: 0 (0x0)
        blocking thread_name: binder:553_3 (0x0)
        blocking process_name: /system/bin/mediaserver (0x0)
        blocking thread_state: Running (0x0)
        binder transaction (0x0)
        bindApplication (0x0)
        thread_name: rs.media.module (0x0)
        process_name: com.android.providers.media.module (0x0)
        thread_state: S (0x0)
        critical path (0x0)
        """))
