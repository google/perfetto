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
            },
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
