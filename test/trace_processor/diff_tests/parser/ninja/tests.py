#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import TestSuite

# The job lines in both fixtures are the same six lines of a real ninja 1.13.2
# build log; only the version in the header differs. ninja writes v6 since
# 1.12 and v7 since 1.13, neither of which changed the layout of a line, so
# both must import to exactly the same slices as a v5 log would.
_JOBS = '''
  "build","worker","ts","dur","name"
  "Build",1,582000000,13000000,"obj/gn/standalone/libc++/deps.stamp"
  "Build",2,582000000,13000000,"obj/gn/standalone/sanitizers/deps.stamp"
  "Build",3,582000000,20000000,"obj/include/perfetto/public/abi/base.stamp"
  "Build",4,583000000,13000000,"obj/include/perfetto/public/protozero.stamp"
  "Build",5,583000000,20000000,"obj/protos/perfetto/protovm/source_set.stamp"
  "Build",6,588000000,16000000,"obj/protos/perfetto/config/ftrace/source_set.stamp"
'''


class NinjaParser(TestSuite):

  def test_ninja_log_v6(self):
    return DiffTestBlueprint(
        trace=Path('build_log_v6.ninja_log'),
        query="""
          select process.name as build, thread.tid as worker,
                 slice.ts, slice.dur, slice.name
          from slice
          join thread_track on slice.track_id = thread_track.id
          join thread using(utid)
          join process using(upid)
          order by slice.ts, slice.name
        """,
        out=Csv(_JOBS))

  def test_ninja_log_v7(self):
    return DiffTestBlueprint(
        trace=Path('build_log_v7.ninja_log'),
        query="""
          select process.name as build, thread.tid as worker,
                 slice.ts, slice.dur, slice.name
          from slice
          join thread_track on slice.track_id = thread_track.id
          join thread using(utid)
          join process using(upid)
          order by slice.ts, slice.name
        """,
        out=Csv(_JOBS))
