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

from python.generators.diff_tests.testing import Csv, Json
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class JsonParser(TestSuite):

  def test_string_pid_tid(self):
    return DiffTestBlueprint(
        trace=Json('''
          {
            "traceEvents": [{
              "pid": "foo",
              "tid": "bar",
              "ts": 5.1,
              "dur": 500.1,
              "name": "name.exec",
              "ph": "XXX",
              "cat": "aaa"
            }]
          }
        '''),
        query="""
          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            process.name as process_name,
            thread.name as thread_name
          FROM slice
          LEFT JOIN thread_track ON slice.track_id = thread_track.id
          LEFT JOIN thread USING (utid)
          LEFT JOIN process USING (upid)
        """,
        out=Csv("""
          "ts","dur","name","process_name","thread_name"
          5100,500100,"name.exec","foo","bar"
        """))

  def test_args_ordered(self):
    # This is a regression test for https://github.com/google/perfetto/issues/553.
    # When importing from JSON, we expect arguments to be ordered.
    #
    # The bug was that we have sorted keys using their interned id when grouping
    # args from different events (e.g. begin / end pair). This was working most
    # of the time (as the key are processed in sorted order and interned ids are
    # incremental).
    #
    # This test, however, is crafted to trigger the bug by ensuring that some
    # keys are seens first (due to being seen in a different event, and therefore
    # being already interned and therefore having a lower interned id.
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "name": "Event1",
              "cat": "C",
              "ph": "b",
              "ts": 40000,
              "pid": 1,
              "id": 1,
              "args": {
                "02.step2": 2,
              }
            },
            {
              "name": "Event2",
              "cat": "C",
              "ph": "b",
              "ts": 40000,
              "pid": 2,
              "id": 1,
              "args": {
                "01.step1": 1,
                "02.step2": 2,
              }
            }
          ]'''),
        query='''
          SELECT
            slice.name,
            args.key,
            args.int_value
          FROM slice
          JOIN args ON slice.arg_set_id = args.arg_set_id
          ORDER BY slice.id, args.id
        ''',
        out=Csv("""
          "name","key","int_value"
          "Event1","args.02.step2",2
          "Event2","args.01.step1",1
          "Event2","args.02.step2",2
        """))

  def test_x_event_order(self):
    return DiffTestBlueprint(
        trace=Json('''[
          {
            "name": "Child",
            "ph": "X",
            "ts": 1,
            "dur": 5,
            "pid": 1
          },
          {
            "name": "Parent",
            "ph": "X",
            "ts": 1,
            "dur": 10,
            "pid": 1,
            "tid": 1
          }
        ]'''),
        query='''
          SELECT ts, dur, name, depth
          FROM slice
        ''',
        out=Csv("""
          "ts","dur","name","depth"
          1000,10000,"Parent",0
          1000,5000,"Child",1
        """))

  def test_json_incomplete(self):
    return DiffTestBlueprint(
        trace=Json('''
        [
        {"name":"typecheck","ph":"X","ts":4619295550.000,"dur":8000.000,"pid":306339,"tid":3},
      '''),
        query='''
        select ts from slice
      ''',
        out=Csv('''
      "ts"
      4619295550000
      '''))

  def test_json_all_slice_types(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "B",
              "pid": 10,
              "tid": 11,
              "ts": 100,
              "name": "BeginEvent",
              "cat": "cat1",
              "args": { "arg_str": "hello", "arg_int": 123, "arg_bool": true }
            },
            {
              "ph": "E",
              "pid": 10,
              "tid": 11,
              "ts": 200,
              "name": "EndEvent",
              "cat": "cat1",
              "tts": 190,
              "args": { "arg_double": 45.67 }
            },
            {
              "ph": "X",
              "pid": 10,
              "tid": 12,
              "ts": 250,
              "dur": 50,
              "name": "CompleteEvent",
              "cat": "cat2,cat3",
              "tts": 240,
              "tdur": 40,
              "args": { "arg_null": null, "another_int": -5 }
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 11,
              "ts": 300,
              "name": "InstantGlobal",
              "cat": "cat_inst",
              "s": "g",
              "args": {}
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 0,
              "ts": 310,
              "name": "InstantProcess",
              "cat": "cat_inst",
              "s": "p",
              "args": { "scope_val": "process" }
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 11,
              "ts": 320,
              "name": "InstantThread",
              "cat": "cat_inst",
              "s": "t"
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 12,
              "ts": 330,
              "name": "InstantDefaultScope",
              "cat": "cat_inst"
            },
            {
              "ph": "R",
              "pid": 10,
              "tid": 11,
              "ts": 340,
              "name": "InstantR",
              "cat": "cat_inst",
              "s": "g"
            },
            {
              "ph": "i",
              "pid": 10,
              "tid": 11,
              "ts": 350,
              "name": "Instanti",
              "cat": "cat_inst",
              "s": "t"
            }
          ]
        '''),
        query='''
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            args.flat_key,
            args.key,
            args.string_value,
            args.int_value
          FROM thread_or_process_slice AS slice
          LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ''',
        out=Csv("""
          "ts","dur","name","flat_key","key","string_value","int_value"
          100000,-1,"BeginEvent","args.arg_str","args.arg_str","hello","[NULL]"
          100000,-1,"BeginEvent","args.arg_int","args.arg_int","[NULL]",123
          100000,-1,"BeginEvent","args.arg_bool","args.arg_bool","[NULL]",1
          320000,0,"InstantThread","[NULL]","[NULL]","[NULL]","[NULL]"
          350000,0,"Instanti","[NULL]","[NULL]","[NULL]","[NULL]"
          250000,50000,"CompleteEvent","args.another_int","args.another_int","[NULL]",-5
          330000,0,"InstantDefaultScope","[NULL]","[NULL]","[NULL]","[NULL]"
          310000,0,"InstantProcess","args.scope_val","args.scope_val","process","[NULL]"
        """))

  def test_json_flow(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "B",
              "pid": 10,
              "tid": 11,
              "ts": 100,
              "name": "BeginEvent",
              "cat": "cat1",
              "args": { "arg_str": "hello", "arg_int": 123, "arg_bool": true }
            },
            {
              "ph": "E",
              "pid": 10,
              "tid": 11,
              "ts": 200,
              "name": "EndEvent",
              "cat": "cat1",
              "tts": 190,
              "args": { "arg_double": 45.67 }
            },
            {
              "ph": "X",
              "pid": 10,
              "tid": 12,
              "ts": 250,
              "dur": 50,
              "name": "CompleteEvent",
              "cat": "cat2,cat3",
              "tts": 240,
              "tdur": 40,
              "args": { "arg_null": null, "another_int": -5 }
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 11,
              "ts": 300,
              "name": "InstantGlobal",
              "cat": "cat_inst",
              "s": "g",
              "args": {}
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 0,
              "ts": 310,
              "name": "InstantProcess",
              "cat": "cat_inst",
              "s": "p",
              "args": { "scope_val": "process" }
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 11,
              "ts": 320,
              "name": "InstantThread",
              "cat": "cat_inst",
              "s": "t"
            },
            {
              "ph": "I",
              "pid": 10,
              "tid": 12,
              "ts": 330,
              "name": "InstantDefaultScope",
              "cat": "cat_inst"
            },
            {
              "ph": "R",
              "pid": 10,
              "tid": 11,
              "ts": 340,
              "name": "InstantR",
              "cat": "cat_inst",
              "s": "g"
            },
            {
              "ph": "i",
              "pid": 10,
              "tid": 11,
              "ts": 350,
              "name": "Instanti",
              "cat": "cat_inst",
              "s": "t"
            }
          ]
        '''),
        query='''
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            COUNT(DISTINCT fin.id) AS fin,
            COUNT(DISTINCT fout.id) AS fout
          FROM thread_or_process_slice AS slice
          LEFT JOIN flow as fin ON slice.id = fin.slice_in
          LEFT JOIN flow as fout ON slice.id = fout.slice_out
        ''',
        out=Csv("""
          "ts","dur","name","fin","fout"
          100000,-1,"BeginEvent",0,0
        """))

  def test_json_metadata(self):
    return DiffTestBlueprint(
        trace=Json('''[
          {
            "ph": "B",
            "pid": 30,
            "tid": 31,
            "ts": 790,
            "name": "ActivityOnThread31",
            "cat": "test_cat_setup"
          },
          {
            "ph": "C",
            "pid": 30,
            "tid": 31,
            "ts": 800,
            "name": "MyCounters",
            "cat": "stats",
            "id": "counter_group_1",
            "args": {
              "counter1_float": 100.5,
              "counter2_int": -50,
              "counter3_str_num": "25.7"
            }
          },
          {
            "ph": "E",
            "pid": 30,
            "tid": 31,
            "ts": 815,
            "name": "ActivityOnThread31",
            "cat": "test_cat_setup"
          },
          {
            "ph": "C",
            "pid": 30,
            "tid": 31,
            "ts": 810,
            "name": "EmptyArgsCounter",
            "cat": "stats",
            "args": {}
          },
          {
            "ph": "C",
            "pid": 30,
            "tid": 31,
            "ts": 820,
            "name": "NoArgsCounter",
            "cat": "stats"
          },
          {
            "ph": "M",
            "pid": 30,
            "tid": 0,
            "ts": 830,
            "name": "process_name",
            "cat": "__metadata",
            "args": { "name": "MyProcess30" }
          },
          {
            "ph": "M",
            "pid": 30,
            "tid": 31,
            "ts": 840,
            "name": "thread_name",
            "cat": "__metadata",
            "args": { "name": "MyRevisedThread31Name" }
          },
          {
            "ph": "M",
            "pid": 30,
            "tid": 0,
            "ts": 850,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": -2 }
          },
          {
            "ph": "M",
            "pid": 30,
            "tid": 31,
            "ts": 860,
            "name": "thread_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 5 }
          },
          {
            "ph": "B",
            "pid": 30,
            "tid": 32,
            "ts": 900,
            "name": "EventMissingCatAndArgs"
          },
          {
            "ph": "E",
            "pid": 30,
            "tid": 32,
            "ts": 950
          },
          {
            "ph": "X",
            "pid": 33,
            "ts": 960,
            "dur": 10,
            "name": "PidOnlyEvent",
            "cat": "special_pids"
          },
          {
            "ph": "X",
            "ts": 970,
            "dur": 10,
            "name": "NoPidNoTidEvent",
            "cat": "special_pids"
          }
        ]

        '''),
        query='''
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            slice.thread_name,
            slice.tid,
            slice.process_name,
            slice.pid,
            args.flat_key,
            args.key,
            args.string_value,
            args.int_value
          FROM thread_or_process_slice AS slice
          LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ''',
        out=Csv("""
          "ts","dur","name","thread_name","tid","process_name","pid","flat_key","key","string_value","int_value"
          790000,25000,"ActivityOnThread31","MyRevisedThread31Name",31,"MyProcess30",30,"[NULL]","[NULL]","[NULL]","[NULL]"
          900000,50000,"EventMissingCatAndArgs","[NULL]",32,"MyProcess30",30,"[NULL]","[NULL]","[NULL]","[NULL]"
          960000,10000,"PidOnlyEvent","[NULL]",33,"[NULL]",33,"[NULL]","[NULL]","[NULL]","[NULL]"
          970000,10000,"NoPidNoTidEvent","[NULL]",0,"[NULL]",0,"[NULL]","[NULL]","[NULL]","[NULL]"
        """))

  def test_json_extreme_vals(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "X",
              "pid": 100,
              "tid": 101,
              "ts": 1000,
              "name": "CompleteNoDur",
              "cat": "test_missing"
            },
            {
              "ph": "B",
              "pid": 100,
              "tid": 102,
              "ts": 1100,
              "name": "BeginNoTTS",
              "cat": "test_missing"
            },
            {
              "ph": "E",
              "pid": 100,
              "tid": 102,
              "ts": 1200,
              "name": "EndNoTTS",
              "cat": "test_missing"
            },
            {
              "ph": "X",
              "pid": 100,
              "tid": 103,
              "ts": 1300,
              "dur": 50,
              "name": "CompleteNoTDUR",
              "cat": "test_missing",
              "tts": 1290
            }
          ]
        '''),
        query='''
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            slice.thread_name,
            slice.tid,
            slice.process_name,
            slice.pid,
            args.flat_key,
            args.key,
            args.string_value,
            args.int_value
          FROM thread_or_process_slice AS slice
          LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ''',
        out=Csv("""
          "ts","dur","name","thread_name","tid","process_name","pid","flat_key","key","string_value","int_value"
          1100000,-1,"BeginNoTTS","[NULL]",102,"[NULL]",100,"[NULL]","[NULL]","[NULL]","[NULL]"
          1300000,50000,"CompleteNoTDUR","[NULL]",103,"[NULL]",100,"[NULL]","[NULL]","[NULL]","[NULL]"
        """))

  def test_json_async(self):
    return DiffTestBlueprint(
        trace=Json('''[
            {
              "ph": "b",
              "pid": 200,
              "tid": 201,
              "ts": 2000,
              "name": "AsyncMissingCat",
              "id": "flow1"
            },
            {
              "ph": "e",
              "pid": 200,
              "tid": 201,
              "ts": 2100,
              "name": "AsyncMissingCat",
              "id": "flow1"
            },
            {
              "ph": "b",
              "pid": 200,
              "tid": 202,
              "ts": 2200,
              "name": "AsyncEmptyStringId",
              "cat": "category2",
              "id": ""
            },
            {
              "ph": "e",
              "pid": 200,
              "tid": 202,
              "ts": 2300,
              "name": "AsyncEmptyStringId",
              "cat": "category2",
              "id": ""
            },
            {
              "ph": "b",
              "pid": 200,
              "tid": 203,
              "ts": 2400,
              "name": "AsyncMissingCatEmptyStringId",
              "id": ""
            },
            {
              "ph": "e",
              "pid": 200,
              "tid": 203,
              "ts": 2500,
              "name": "AsyncMissingCatEmptyStringId",
              "id": ""
            }
          ]
        '''),
        query='''
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            slice.thread_name,
            slice.tid,
            slice.process_name,
            slice.pid,
            args.flat_key,
            args.key,
            args.string_value,
            args.int_value
          FROM thread_or_process_slice AS slice
          LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ''',
        out=Csv("""
          "ts","dur","name","thread_name","tid","process_name","pid","flat_key","key","string_value","int_value"
        """))

  def test_json_counter_args(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "C",
              "pid": 300,
              "tid": 301,
              "ts": 3000,
              "name": "CounterStringEdgeCases",
              "cat": "counters_str",
              "args": {
                "leading_dot": ".75",
                "trailing_dot": "246.",
                "leading_zero_float": "01.23",
                "leading_zero_int_str": "050",
                "incomplete_exp": "1.5e",
                "exp_no_digit": "2e+",
                "just_dot": ".",
                "plus_val": "+10.5",
                "just_plus": "+",
                "just_minus": "-"
              }
            }
          ]
        '''),
        query='''
          SELECT process.pid, process.name, pct.name, value
          FROM counter
          JOIN process_counter_track pct ON counter.track_id = pct.id
          JOIN process USING (upid)
          ORDER BY pct.name
        ''',
        out=Csv("""
          "pid","name","name","value"
          300,"[NULL]","CounterStringEdgeCases leading_dot",0.750000
          300,"[NULL]","CounterStringEdgeCases leading_zero_float",1.230000
          300,"[NULL]","CounterStringEdgeCases leading_zero_int_str",50.000000
          300,"[NULL]","CounterStringEdgeCases plus_val",10.500000
          300,"[NULL]","CounterStringEdgeCases trailing_dot",246.000000
        """))

  def test_json_trailing_comma(self):
    return DiffTestBlueprint(
        trace=Json('''
          {"displayTimeUnit":"ms","traceEvents":[
          {"name":"Foo","ph":"X","tid":0,"ts":3473608.458,"dur":295555.500},
          {"name":"Bar","ph":"X","tid":0,"ts":4890.000,"dur":3764289.708},
          ]}
        '''),
        query="""
          SELECT
            slice.name,
            slice.ts,
            slice.dur
          FROM slice
          ORDER BY slice.ts
        """,
        out=Csv("""
          "name","ts","dur"
          "Bar",4890000,3764289708
          "Foo",3473608458,295555500
        """))

  def test_json_id2(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "b",
              "pid": 400,
              "tid": 401,
              "ts": 4000,
              "name": "AsyncId2StandardGlobalKey",
              "cat": "id2test",
              "id2": {
                "global": "standard_global_id_1"
              }
            },
            {
              "ph": "e",
              "pid": 400,
              "tid": 401,
              "ts": 4100,
              "name": "AsyncId2StandardGlobalKey",
              "cat": "id2test",
              "id2": {
                "global": "standard_global_id_1"
              }
            },
            {
              "ph": "b",
              "pid": 400,
              "tid": 402,
              "ts": 4200,
              "name": "AsyncId2EmptyGlobalKey",
              "cat": "id2test",
              "id2": {
                "": "empty_key_global_id_2"
              }
            },
            {
              "ph": "e",
              "pid": 400,
              "tid": 402,
              "ts": 4300,
              "name": "AsyncId2EmptyGlobalKey",
              "cat": "id2test",
              "id2": {
                "": "empty_key_global_id_2"
              }
            }
          ]
        '''),
        query='''
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            slice.thread_name,
            slice.tid,
            slice.process_name,
            slice.pid,
            args.flat_key,
            args.key,
            args.string_value,
            args.int_value
          FROM thread_or_process_slice AS slice
          LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ''',
        out=Csv("""
          "ts","dur","name","thread_name","tid","process_name","pid","flat_key","key","string_value","int_value"
        """))

  def test_string_ts_and_dur(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "B",
              "pid": 10,
              "tid": 11,
              "ts": "100",
              "name": "BeginEvent"
            },
            {
              "ph": "E",
              "pid": 10,
              "tid": 11,
              "ts": "200.5",
              "name": "BeginEvent"
            },
            {
              "ph": "X",
              "pid": 10,
              "tid": 12,
              "ts": "250",
              "dur": "50.5",
              "name": "CompleteEvent",
              "cat": "cat2,cat3",
              "tts": "240",
              "tdur": "40.5"
            }
          ]
        '''),
        query='''
          SELECT
            slice.ts,
            slice.dur,
            slice.name
          FROM slice
          ORDER BY ts
        ''',
        out=Csv("""
          "ts","dur","name"
          100000,100500,"BeginEvent"
          250000,50500,"CompleteEvent"
        """))

  def test_invalid_string_ts_and_dur(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "X",
              "pid": 10,
              "tid": 13,
              "ts": "300",
              "dur": "invalid",
              "name": "InvalidDur"
            },
            {
              "ph": "X",
              "pid": 10,
              "tid": 14,
              "ts": "invalid",
              "dur": "50",
              "name": "InvalidTs"
            }
          ]
        '''),
        query='''
          SELECT
            slice.ts,
            slice.dur,
            slice.name
          FROM slice
          ORDER BY ts
        ''',
        out=Csv("""
          "ts","dur","name"
        """))

  def test_string_tts_and_tdur(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "X",
              "pid": 10,
              "tid": 15,
              "ts": "400",
              "dur": "100",
              "tts": "390.5",
              "tdur": "80.5",
              "name": "StringTtsTdur"
            }
          ]
        '''),
        query='''
          SELECT
            slice.ts,
            slice.dur,
            slice.name,
            slice.thread_ts,
            slice.thread_dur
          FROM slice
          ORDER BY ts
        ''',
        out=Csv("""
          "ts","dur","name","thread_ts","thread_dur"
          400000,100000,"StringTtsTdur",390500,80500
        """))

  def test_json_id2_global_string_id(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "b",
              "pid": 400,
              "tid": 401,
              "ts": 4000,
              "name": "AsyncId2GlobalString",
              "cat": "id2test",
              "id2": {
                "global": "global_id_str"
              }
            },
            {
              "ph": "e",
              "pid": 400,
              "tid": 401,
              "ts": 4100,
              "name": "AsyncId2GlobalString",
              "cat": "id2test",
              "id2": {
                "global": "global_id_str"
              }
            }
          ]
        '''),
        query='''
          SELECT
            slice.name,
            slice.ts,
            slice.dur,
            track.name as track_name,
            track.type as track_type
          FROM slice
          JOIN track on slice.track_id = track.id
          WHERE slice.name = "AsyncId2GlobalString"
        ''',
        out=Csv("""
          "name","ts","dur","track_name","track_type"
          "AsyncId2GlobalString",4000000,100000,"AsyncId2GlobalString","legacy_async_global_slice"
        """))

  def test_json_id2_local_string_id(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "b",
              "pid": 400,
              "tid": 401,
              "ts": 4000,
              "name": "AsyncId2LocalString",
              "cat": "id2test",
              "id2": {
                "local": "local_id_str"
              }
            },
            {
              "ph": "e",
              "pid": 400,
              "tid": 401,
              "ts": 4100,
              "name": "AsyncId2LocalString",
              "cat": "id2test",
              "id2": {
                "local": "local_id_str"
              }
            }
          ]
        '''),
        query='''
          SELECT
            slice.name,
            slice.ts,
            slice.dur,
            track.name as track_name,
            track.type as track_type
          FROM slice
          JOIN track on slice.track_id = track.id
          WHERE slice.name = "AsyncId2LocalString"
        ''',
        out=Csv("""
          "name","ts","dur","track_name","track_type"
          "AsyncId2LocalString",4000000,100000,"AsyncId2LocalString","legacy_async_process_slice"
        """))

  def test_string_ts_trailing_chars(self):
    return DiffTestBlueprint(
        trace=Json('''
          [
            {
              "ph": "X",
              "pid": 10,
              "tid": 16,
              "ts": "100a",
              "dur": "50",
              "name": "TrailingCharsTs"
            },
            {
              "ph": "X",
              "pid": 10,
              "tid": 17,
              "ts": "200",
              "dur": "50a",
              "name": "TrailingCharsDur"
            }
          ]
        '''),
        query='''
          SELECT
            slice.name,
            slice.ts,
            slice.dur
          FROM slice
        ''',
        out=Csv("""
          "name","ts","dur"
        """))

  def test_json_id2_global_int_id(self):
    return DiffTestBlueprint(
        trace=Json('''
          {
              "traceEvents": [
                  {
                      "name": "process_name",
                      "ph": "M",
                      "pid": 2,
                      "args": {
                          "name": "device2"
                      }
                  },
                  {
                      "name": "thread_name",
                      "ph": "M",
                      "pid": 2,
                      "tid": 2,
                      "args": {
                          "name": "send2"
                      }
                  },
                  {
                      "name": "write",
                      "ph": "b",
                      "pid": 2,
                      "tid": 2,
                      "ts": 1850244461563845.0,
                      "id2": {
                          "global": 1
                      },
                      "args": {
                          "dev_name": "86",
                          "wr": "129010217631161",
                          "op_type": "3",
                          "src_num": "1",
                          "dst_num": "3",
                          "ah_num": "1",
                          "length": "1048577"
                      }
                  },
                  {
                      "name": "write",
                      "ph": "e",
                      "pid": 2,
                      "tid": 2,
                      "ts": 1850244461564012.0,
                      "id2": {
                          "global": 1
                      }
                  }
              ]
          }
        '''),
        query='''
          SELECT
            slice.name,
            slice.ts,
            slice.dur,
            track.name as track_name,
            track.type as track_type
          FROM slice
          JOIN track on slice.track_id = track.id
          WHERE slice.name = "write"
        ''',
        out=Csv("""
          "name","ts","dur","track_name","track_type"
          "write",1850244461563845000,167000,"write","legacy_async_global_slice"
        """))

  def test_json_id2_local_int_id(self):
    return DiffTestBlueprint(
        trace=Json('''
          {
              "traceEvents": [
                  {
                      "name": "process_name",
                      "ph": "M",
                      "pid": 1,
                      "args": {
                          "name": "device"
                      }
                  },
                  {
                      "name": "thread_name",
                      "ph": "M",
                      "pid": 1,
                      "tid": 1,
                      "args": {
                          "name": "send"
                      }
                  },
                  {
                      "name": "write",
                      "ph": "b",
                      "pid": 1,
                      "tid": 1,
                      "ts": 1750244461563845.0,
                      "id2": {
                          "local": 0
                      },
                      "args": {
                          "dev_name": "85",
                          "wr": "129010217631160",
                          "op_type": "2",
                          "src_num": "0",
                          "dst_num": "2",
                          "ah_num": "0",
                          "length": "1048576"
                      }
                  },
                  {
                      "name": "write",
                      "ph": "e",
                      "pid": 1,
                      "tid": 1,
                      "ts": 1750244461564012.0,
                      "id2": {
                          "local": 0
                      }
                  }
              ]
          }
        '''),
        query='''
          SELECT
            slice.name,
            slice.ts,
            slice.dur,
            track.name as track_name,
            track.type as track_type
          FROM slice
          JOIN track on slice.track_id = track.id
          WHERE slice.name = "write"
        ''',
        out=Csv("""
          "name","ts","dur","track_name","track_type"
          "write",1750244461563845000,167000,"write","legacy_async_process_slice"
        """))

  def test_json_sort_index_hints(self):
    return DiffTestBlueprint(
        trace=Json('''[
          {
            "ph": "M",
            "pid": 30,
            "tid": 0,
            "ts": 100,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": -2 }
          },
          {
            "ph": "M",
            "pid": 30,
            "tid": 31,
            "ts": 110,
            "name": "thread_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 5 }
          },
          {
            "ph": "M",
            "pid": 40,
            "tid": 0,
            "ts": 120,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 10 }
          },
          {
            "ph": "X",
            "pid": 30,
            "tid": 31,
            "ts": 200,
            "dur": 100,
            "name": "Event1"
          }
        ]'''),
        query='''
          SELECT
            process.pid,
            thread.tid,
            extract_arg(process.arg_set_id, 'process_sort_index_hint') as process_hint,
            extract_arg(thread.arg_set_id, 'thread_sort_index_hint') as thread_hint
          FROM thread
          JOIN process USING (upid)
          ORDER BY process.pid, thread.tid
        ''',
        out=Csv("""
          "pid","tid","process_hint","thread_hint"
          0,0,"[NULL]","[NULL]"
          30,30,-2,"[NULL]"
          30,31,-2,5
          40,40,10,"[NULL]"
        """))

  def test_json_sort_index_ordering(self):
    return DiffTestBlueprint(
        trace=Json('''[
          {
            "ph": "M",
            "pid": 10,
            "tid": 10,
            "ts": 100,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 100 }
          },
          {
            "ph": "M",
            "pid": 20,
            "tid": 20,
            "ts": 110,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 50 }
          },
          {
            "ph": "M",
            "pid": 30,
            "tid": 30,
            "ts": 120,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 200 }
          },
          {
            "ph": "M",
            "pid": 40,
            "tid": 40,
            "ts": 130,
            "name": "process_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": -5 }
          },
          {
            "ph": "M",
            "pid": 10,
            "tid": 11,
            "ts": 200,
            "name": "thread_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 10 }
          },
          {
            "ph": "M",
            "pid": 10,
            "tid": 12,
            "ts": 210,
            "name": "thread_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 20 }
          },
          {
            "ph": "M",
            "pid": 10,
            "tid": 13,
            "ts": 220,
            "name": "thread_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": 5 }
          },
          {
            "ph": "M",
            "pid": 10,
            "tid": 14,
            "ts": 230,
            "name": "thread_sort_index",
            "cat": "__metadata",
            "args": { "sort_index": -3 }
          },
          {
            "ph": "X",
            "pid": 10,
            "tid": 11,
            "ts": 1000,
            "dur": 100,
            "name": "Event1"
          },
          {
            "ph": "X",
            "pid": 20,
            "tid": 20,
            "ts": 2000,
            "dur": 100,
            "name": "Event2"
          },
          {
            "ph": "X",
            "pid": 30,
            "tid": 30,
            "ts": 3000,
            "dur": 100,
            "name": "Event3"
          },
          {
            "ph": "X",
            "pid": 40,
            "tid": 40,
            "ts": 4000,
            "dur": 100,
            "name": "Event4"
          }
        ]'''),
        query='''
          WITH process_ordering AS (
            SELECT
              process.pid,
              ifnull(extract_arg(process.arg_set_id, 'process_sort_index_hint'), 0) as process_hint
            FROM process
            ORDER BY
              process_hint asc,
              process.pid asc
          ),
          thread_ordering AS (
            SELECT
              thread.tid,
              thread.upid,
              ifnull(extract_arg(thread.arg_set_id, 'thread_sort_index_hint'), 0) as thread_hint
            FROM thread
            WHERE upid = (SELECT upid FROM process WHERE pid = 10)
            ORDER BY
              thread_hint asc,
              thread.tid asc
          )
          SELECT 'process' as type, pid as id, process_hint as hint FROM process_ordering
          UNION ALL
          SELECT 'thread' as type, tid as id, thread_hint as hint FROM thread_ordering
        ''',
        out=Csv("""
          "type","id","hint"
          "process",40,-5
          "process",0,0
          "process",20,50
          "process",10,100
          "process",30,200
          "thread",14,-3
          "thread",10,0
          "thread",13,5
          "thread",11,10
          "thread",12,20
        """))

  def test_json_pid_tid_zero(self):
    return DiffTestBlueprint(
        trace=Json('''
          {
            "traceEvents": [
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 0,
                "ph": "b",
                "name": "RoutineControl Reeq",
                "id": "0x31",
                "args": {
                  "name": "RoutineControl Reeq",
                  "detail": "RoutineControl Reeq",
                  "hex": "31 01 02 03 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 0,
                "ph": "b",
                "name": "31 ",
                "id": "0x31",
                "args": {
                  "name": "31 ",
                  "detail": "RoutineControl",
                  "hex": "31 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 1,
                "ph": "e",
                "name": "31 ",
                "id": "0x31",
                "args": {
                  "name": "31 ",
                  "detail": "RoutineControl",
                  "hex": "31 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 0,
                "ph": "b",
                "name": "RoutineControl",
                "id": "0x31",
                "args": {
                  "name": "RoutineControl",
                  "detail": "RoutineControl",
                  "hex": "31 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 1,
                "ph": "e",
                "name": "RoutineControl",
                "id": "0x31",
                "args": {
                  "name": "RoutineControl",
                  "detail": "RoutineControl",
                  "hex": "31 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 1,
                "ph": "b",
                "name": "01 ",
                "id": "0x31",
                "args": {
                  "name": "01 ",
                  "detail": "startRoutine",
                  "hex": "01 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 2,
                "ph": "e",
                "name": "01 ",
                "id": "0x31",
                "args": {
                  "name": "01 ",
                  "detail": "startRoutine",
                  "hex": "01 ",
                  "timestamp": "20250704_194952_426",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 1,
                "ph": "b",
                "name": "startRoutine",
                "id": "0x31",
                "args": {
                  "name": "startRoutine",
                  "detail": "startRoutine",
                  "hex": "01 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 2,
                "ph": "e",
                "name": "startRoutine",
                "id": "0x31",
                "args": {
                  "name": "startRoutine",
                  "detail": "startRoutine",
                  "hex": "01 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 2,
                "ph": "b",
                "name": "02 03 ",
                "id": "0x31",
                "args": {
                  "name": "02 03 ",
                  "detail": "Routine Identifier",
                  "hex": "02 03 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 4,
                "ph": "e",
                "name": "02 03 ",
                "id": "0x31",
                "args": {
                  "name": "02 03 ",
                  "detail": "Routine Identifier",
                  "hex": "02 03 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 2,
                "ph": "b",
                "name": "Routine Identifier",
                "id": "0x31",
                "args": {
                  "name": "Routine Identifier",
                  "detail": "Routine Identifier",
                  "hex": "02 03 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 4,
                "ph": "e",
                "name": "Routine Identifier",
                "id": "0x31",
                "args": {
                  "name": "Routine Identifier",
                  "detail": "Routine Identifier",
                  "hex": "02 03 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              },
              {
                "cat": "Reeq",
                "pid": 0,
                "tid": 0,
                "ts": 4,
                "ph": "e",
                "name": "RoutineControl Reeq",
                "id": "0x31",
                "args": {
                  "name": "RoutineControl Reeq",
                  "detail": "RoutineControl Reeq",
                  "hex": "31 01 02 03 ",
                  "timestamp": "20250704_194952_427",
                  "raw": "ID: 760, DL: 08, 04 31 01 02 03 cc cc cc \n"
                }
              }
            ]
          }
        '''),
        query='''
          SELECT name, ts, dur FROM slice
        ''',
        out=Csv("""
          "name","ts","dur"
          "RoutineControl Reeq",0,4000
          "31 ",0,1000
          "RoutineControl",0,1000
          "01 ",1000,1000
          "startRoutine",1000,1000
          "02 03 ",2000,2000
          "Routine Identifier",2000,2000
        """))
