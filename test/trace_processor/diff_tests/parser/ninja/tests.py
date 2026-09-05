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

  # A ninja log accumulates the logs of every invocation in that output
  # directory, and each invocation restarts its timestamps from zero. The
  # importer must not lay them on top of each other: it infers parallelism
  # from overlapping timestamps, so superimposed builds look like one build
  # with several times the real number of workers. Here the second build (the
  # last line, which is what breaks the monotonicity of the end timestamps) is
  # shifted past the end of the first, and its job lands back on the worker
  # that ran obj/a.o rather than on a fourth one.
  def test_ninja_log_two_builds(self):
    return DiffTestBlueprint(
        trace=Path('build_log_two_builds.ninja_log'),
        query="""
          select thread.name as worker, slice.ts, slice.dur, slice.name
          from slice
          join thread_track on slice.track_id = thread_track.id
          join thread using(utid)
          order by slice.ts, slice.name
        """,
        out=Csv("""
          "worker","ts","dur","name"
          "Worker 1",0,100000000,"obj/a.o"
          "Worker 2",0,100000000,"obj/b.o"
          "Worker 1",100000000,100000000,"obj/c.o"
          "Worker 1",200000000,50000000,"obj/a.o"
        """))

  # A rule with several outputs is written as one line per output, all with
  # the same command hash and the same timestamps. Those are one invocation of
  # one tool and must become a single slice, not one slice per output on a
  # worker of its own.
  def test_ninja_log_multi_output(self):
    return DiffTestBlueprint(
        trace=Path('build_log_multi_output.ninja_log'),
        query="""
          select thread.name as worker, slice.ts, slice.dur, slice.name
          from slice
          join thread_track on slice.track_id = thread_track.id
          join thread using(utid)
          order by slice.ts, slice.name
        """,
        out=Csv("""
          "worker","ts","dur","name"
          "Worker 1",0,100000000,"gen/foo.pb.h, gen/foo.pb.cc"
          "Worker 1",120000000,60000000,"obj/foo.o"
        """))
