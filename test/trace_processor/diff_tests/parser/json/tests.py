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
          100000,-1,"BeginEvent","args.arg_bool","args.arg_bool","[NULL]",1
          100000,-1,"BeginEvent","args.arg_int","args.arg_int","[NULL]",123
          100000,-1,"BeginEvent","args.arg_str","args.arg_str","hello","[NULL]"
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
