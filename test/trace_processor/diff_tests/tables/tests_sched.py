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
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
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

  def test_sched_waker_id(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        SELECT parent.id
        FROM thread_state parent
        JOIN thread_state child
          ON parent.utid = child.waker_utid AND child.ts BETWEEN parent.ts AND parent.ts + parent.dur
        WHERE child.id = 15750
        UNION ALL
        SELECT waker_id AS id FROM thread_state WHERE id = 15750
        """,
        out=Csv("""
        "id"
        15748
        15748
        """))

  def test_raw_common_flags(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        SELECT id, type, ts, name, cpu, utid, arg_set_id, common_flags
        FROM raw WHERE common_flags != 0 ORDER BY ts LIMIT 10
        """,
        out=Csv("""
        "id","type","ts","name","cpu","utid","arg_set_id","common_flags"
        3,"__intrinsic_ftrace_event",1735489788930,"sched_waking",0,300,4,1
        4,"__intrinsic_ftrace_event",1735489812571,"sched_waking",0,300,5,1
        5,"__intrinsic_ftrace_event",1735489833977,"sched_waking",1,305,6,1
        8,"__intrinsic_ftrace_event",1735489876788,"sched_waking",1,297,9,1
        9,"__intrinsic_ftrace_event",1735489879097,"sched_waking",0,304,10,1
        12,"__intrinsic_ftrace_event",1735489933912,"sched_waking",0,428,13,1
        14,"__intrinsic_ftrace_event",1735489972385,"sched_waking",1,232,15,1
        17,"__intrinsic_ftrace_event",1735489999987,"sched_waking",1,232,15,1
        19,"__intrinsic_ftrace_event",1735490039439,"sched_waking",1,298,18,1
        20,"__intrinsic_ftrace_event",1735490042084,"sched_waking",1,298,19,1
        """))

  def test_thread_executing_span_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;
        SELECT
          waker_id,
          prev_id,
          ts - idle_dur AS idle_ts,
          id,
          ts,
          ts + dur AS next_idle_ts ,
          is_idle_reason_self,
          utid,
          idle_state,
          idle_reason
        FROM _wakeup_graph
        ORDER BY ts
        LIMIT 10
        """,
        out=Csv("""
        "waker_id","prev_id","idle_ts","id","ts","next_idle_ts","is_idle_reason_self","utid","idle_state","idle_reason"
        "[NULL]","[NULL]","[NULL]",5,1735489812571,1735489896509,1,304,"[NULL]","[NULL]"
        "[NULL]","[NULL]","[NULL]",6,1735489833977,1735489886440,1,297,"[NULL]","[NULL]"
        6,"[NULL]","[NULL]",11,1735489876788,1735489953773,0,428,"[NULL]","[NULL]"
        5,"[NULL]","[NULL]",12,1735489879097,1735490217277,0,243,"[NULL]","[NULL]"
        11,"[NULL]","[NULL]",17,1735489933912,1735490587658,0,230,"[NULL]","[NULL]"
        "[NULL]","[NULL]","[NULL]",20,1735489972385,1735489995809,1,298,"[NULL]","[NULL]"
        "[NULL]",20,1735489995809,25,1735489999987,1735490055966,1,298,"S","[NULL]"
        25,"[NULL]","[NULL]",28,1735490039439,1735490610238,0,421,"[NULL]","[NULL]"
        25,"[NULL]","[NULL]",29,1735490042084,1735490068213,0,420,"[NULL]","[NULL]"
        25,"[NULL]","[NULL]",30,1735490045825,1735491418790,0,1,"[NULL]","[NULL]"
        """))

  def test_thread_executing_span_graph_contains_forked_states(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;
        SELECT
          id,
          waker_id,
          prev_id
        FROM _wakeup_graph
          WHERE ts = 1735842081507 AND ts + dur = 1735842081507 + 293868
        """,
        out=Csv("""
        "id","waker_id","prev_id"
        376,369,"[NULL]"
        """))

  def test_thread_executing_span_runnable_state_has_no_running(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;
        SELECT COUNT(*) AS count FROM _runnable_state WHERE state = 'Running'
        """,
        out=Csv("""
        "count"
        0
        """))

  def test_thread_executing_span_graph_has_no_null_dur(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;
        SELECT ts,dur FROM _wakeup_graph
          WHERE dur IS NULL OR ts IS NULL
        """,
        out=Csv("""
        "ts","dur"
        """))

  def test_thread_executing_span_graph_accepts_null_irq_context(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;
        SELECT COUNT(*) AS count FROM _wakeup_graph
        """,
        out=Csv("""
        "count"
        30
        """))

  def test_thread_executing_span_intervals_to_roots_edge_case(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;

        SELECT * FROM
        _intervals_to_roots!((SELECT 1477 AS utid, trace_start() AS ts, trace_end() - trace_start() AS dur), _wakeup_graph)
        ORDER BY root_node_id
        LIMIT 10;
        """,
        out=Csv("""
        "root_node_id","capacity"
        11889,0
        11980,91
        12057,77
        12254,197
        12521,267
        12672,151
        12796,124
        12802,6
        12827,25
        12833,6
        """))

  def test_thread_executing_span_intervals_to_roots(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;

        SELECT * FROM
        _intervals_to_roots!((SELECT 1477 AS utid, 1737362149192 AS ts, CAST(2e7 AS INT) AS dur), _wakeup_graph)
        ORDER BY root_node_id
        LIMIT 10;
        """,
        out=Csv("""
        "root_node_id","capacity"
        11980,91
        12057,77
        12254,197
        """))

  def test_thread_executing_span_flatten_critical_paths(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;

        CREATE PERFETTO TABLE graph AS
        SELECT
          id AS source_node_id,
          COALESCE(waker_id, id) AS dest_node_id,
          id - COALESCE(waker_id, id) AS edge_weight
        FROM _wakeup_graph;

        CREATE PERFETTO TABLE roots AS
        SELECT
          _wakeup_graph.id AS root_node_id,
          _wakeup_graph.id - COALESCE(prev_id, _wakeup_graph.id) AS root_target_weight,
          id,
          ts,
          dur,
          utid
        FROM _wakeup_graph;

        CREATE PERFETTO TABLE critical_path AS
        SELECT root_node_id AS root_id, node_id AS id, root_node_id AS parent_id FROM graph_reachable_weight_bounded_dfs!(graph, roots, 1);

        SELECT * FROM _critical_path_to_intervals!(critical_path, _wakeup_graph);
        """,
        out=Csv("""
        "ts","dur","id","root_id"
        807082871764903,14688,35,38
        807082871805424,6817657,38,45
        807082947223556,23282,60,62
        807082947156994,351302,57,76
        807082947593348,4229115,76,96
        807082959078401,95105,105,107
        807082951886890,79702873,1,130
        807083031589763,324114,127,130
        807082947219546,85059279,1,135
        """))

  def test_thread_executing_span_critical_path(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;

        SELECT * FROM _critical_path_intervals!(_wakeup_kernel_edges, (SELECT id AS root_node_id, id - COALESCE(prev_id, id)  AS capacity FROM _wakeup_graph), _wakeup_graph) ORDER BY root_id;
        """,
        out=Csv("""
        "root_id","id","ts","dur"
        1,1,807082862885423,169601892
        2,2,807082862913183,280521
        13,13,807082864992767,6772136
        14,14,807082865019382,14160157
        17,17,807082865084902,272865
        29,29,807082868359903,81302
        35,35,807082871734539,70885
        38,35,807082871764903,14688
        38,38,807082871779591,6869792
        45,38,807082871805424,6817657
        45,45,807082878623081,242864
        55,55,807082946856213,609219
        57,57,807082947156994,436354
        60,60,807082947223556,83577300
        62,60,807082947223556,23282
        62,62,807082947246838,2000260
        63,63,807082947261525,293594
        64,64,807082947267463,228958
        65,65,807082947278140,54114
        66,66,807082947288765,338802
        67,67,807082947294182,296875
        76,57,807082947156994,351302
        76,76,807082947508296,4378594
        93,93,807082951711161,2494011
        96,76,807082947593348,4229115
        96,96,807082951822463,104427
        105,105,807082959078401,184115
        107,105,807082959078401,95105
        107,107,807082959173506,73362507
        111,111,807082962662412,149011
        114,114,807082967942309,334114
        127,127,807083031589763,436198
        130,1,807082951886890,79702873
        130,127,807083031589763,324114
        130,130,807083031913877,166302
        135,1,807082947219546,85059279
        135,135,807083032278825,208490
        139,139,807083032634138,340625
        142,142,807083032991378,89218
        """))

  def test_thread_executing_span_critical_path_by_roots(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;

        SELECT *
        FROM _critical_path_by_roots!(
          _intervals_to_roots!(
            (SELECT 6 AS utid, trace_start() AS ts, trace_end() - trace_start() AS dur),
            _wakeup_graph),
          _wakeup_graph);
        """,
        out=Csv("""
        "root_id","id","ts","dur"
        14,14,807082865019382,14160157
        62,60,807082947223556,23282
        62,62,807082947246838,2000260
        107,105,807082959078401,95105
        107,139,807082959173506,73362507
        139,139,807083032536013,98125
        139,142,807083032634138,340625
        142,142,807083032974763,16615
        142,142,807083032991378,89218
        """))

  def test_thread_executing_span_critical_path_by_intervals(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;

        SELECT root_utid, root_id, id, ts, dur, utid
        FROM _critical_path_by_intervals!(
          (SELECT 6 AS utid, trace_start() AS ts, trace_end() - trace_start() AS dur),
          _wakeup_graph)
        ORDER BY root_id, ts;
        """,
        out=Csv("""
        "root_utid","root_id","id","ts","dur","utid"
        6,14,14,807082865019382,14160157,6
        6,62,60,807082947223556,23282,11
        6,62,62,807082947246838,2000260,6
        6,107,105,807082959078401,95105,18
        6,107,139,807082959173506,73362507,6
        6,139,139,807083032536013,98125,6
        6,139,142,807083032634138,340625,6
        6,142,142,807083032974763,16615,6
        6,142,142,807083032991378,89218,6
        """))

  def test_thread_executing_span_critical_path_utid(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span;
        SELECT
          root_id,
          root_utid,
          id,
          ts,
          dur,
          utid
        FROM _thread_executing_span_critical_path((select utid from thread where tid = 3487), start_ts, end_ts), trace_bounds
        ORDER BY ts
        LIMIT 10
        """,
        out=Csv("""
        "root_id","root_utid","id","ts","dur","utid"
        11889,1477,11889,1737349401439,7705561,1477
        11980,1477,11952,1737357107000,547583,1480
        11980,1477,11980,1737357654583,8430762,1477
        12057,1477,12052,1737366085345,50400,91
        12057,1477,12057,1737366135745,6635927,1477
        12254,1477,12251,1737372771672,12594070,1488
        12254,1477,12251,1737385365742,204244,1488
        12254,1477,12254,1737385569986,21830622,1477
        12521,1477,12517,1737407400608,241267,91
        12521,1477,12521,1737407641875,1830015,1477
        """))

  def test_thread_executing_span_critical_path_stack(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span_with_slice;
        SELECT
          id,
          ts,
          dur,
          utid,
          stack_depth,
          name,
          table_name,
          root_utid
        FROM _thread_executing_span_critical_path_stack((select utid from thread where tid = 3487), start_ts, end_ts), trace_bounds
        WHERE ts = 1737500355691
        ORDER BY utid, id
        """,
        out=Csv("""
        "id","ts","dur","utid","stack_depth","name","table_name","root_utid"
        4271,1737500355691,1456753,1477,5,"bindApplication","slice",1477
        13120,1737500355691,1456753,1477,0,"thread_state: S","thread_state",1477
        13120,1737500355691,1456753,1477,1,"[NULL]","thread_state",1477
        13120,1737500355691,1456753,1477,2,"[NULL]","thread_state",1477
        13120,1737500355691,1456753,1477,3,"process_name: com.android.providers.media.module","thread_state",1477
        13120,1737500355691,1456753,1477,4,"thread_name: rs.media.module","thread_state",1477
        4800,1737500355691,1456753,1498,11,"HIDL::IComponentStore::getStructDescriptors::client","slice",1477
        4801,1737500355691,1456753,1498,12,"binder transaction","slice",1477
        13648,1737500355691,1456753,1498,6,"blocking thread_state: R+","thread_state",1477
        13648,1737500355691,1456753,1498,7,"blocking process_name: com.android.providers.media.module","thread_state",1477
        13648,1737500355691,1456753,1498,8,"blocking thread_name: CodecLooper","thread_state",1477
        13648,1737500355691,1456753,1498,9,"[NULL]","thread_state",1477
        13648,1737500355691,1456753,1498,10,"[NULL]","thread_state",1477
        """))

  def test_thread_executing_span_critical_path_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_executing_span_with_slice;
        SELECT HEX(pprof) FROM _thread_executing_span_critical_path_graph("critical path", (select utid from thread where tid = 3487), 1737488133487, 16000), trace_bounds
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

  # Test machine_id ID of the sched table.
  def test_android_sched_and_ps_machine_id(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        trace_modifier=TraceInjector(['ftrace_events'], {'machine_id': 1001}),
        query="""
        SELECT ts, cpu.cpu, machine_id
        FROM sched LEFT JOIN cpu USING (ucpu)
        WHERE ts >= 81473797418963 LIMIT 10;
        """,
        out=Csv("""
        "ts","cpu","machine_id"
        81473797824982,3,1
        81473797942847,3,1
        81473798135399,0,1
        81473798786857,2,1
        81473798875451,3,1
        81473799019930,2,1
        81473799079982,0,1
        81473800089357,3,1
        81473800144461,3,1
        81473800441805,3,1
        """))

  # Test the support of machine_id ID of the raw table.
  def test_raw_machine_id(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        trace_modifier=TraceInjector(['ftrace_events'], {'machine_id': 1001}),
        query="""
        SELECT count(*)
        FROM raw LEFT JOIN cpu USING (ucpu)
        WHERE machine_id is NULL;
        """,
        out=Csv("""
        "count(*)"
        0
        """))

  def test_sched_cpu_id(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        SELECT cpu, cluster_id
        FROM cpu
        """,
        out=Csv("""
        "cpu","cluster_id"
        0,0
        1,0
        2,0
        3,0
        4,0
        7,0
        """))

  def test_sched_cpu_id_machine_id(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        trace_modifier=TraceInjector(['ftrace_events'], {'machine_id': 1001}),
        query="""
        SELECT cpu, cluster_id, machine.raw_id as raw_machine_id
        FROM cpu
        JOIN machine ON cpu.machine_id = machine.id
        """,
        out=Csv("""
        "cpu","cluster_id","raw_machine_id"
        0,0,1001
        1,0,1001
        2,0,1001
        3,0,1001
        4,0,1001
        7,0,1001
        """))
