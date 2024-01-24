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


class JsonTests(TestSuite):

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
