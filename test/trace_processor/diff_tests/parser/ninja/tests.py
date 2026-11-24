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
  "worker","ts","dur","name"
  "Worker 1",582000000,13000000,"obj/gn/standalone/libc++/deps.stamp"
  "Worker 2",582000000,13000000,"obj/gn/standalone/sanitizers/deps.stamp"
  "Worker 3",582000000,20000000,"obj/include/perfetto/public/abi/base.stamp"
  "Worker 4",583000000,13000000,"obj/include/perfetto/public/protozero.stamp"
  "Worker 5",583000000,20000000,"obj/protos/perfetto/protovm/source_set.stamp"
  "Worker 6",588000000,16000000,"obj/protos/perfetto/config/ftrace/source_set.stamp"
'''

# The jobs of the log, each with the name of the track it is drawn on. Those
# tracks belong to no process or thread: the name is all the UI shows.
_JOBS_QUERY = '''
  select track.name as worker, slice.ts, slice.dur, slice.name
  from slice
  join track on slice.track_id = track.id
  where track.type = 'ninja_worker'
  order by slice.ts, slice.name
'''


class NinjaParser(TestSuite):

  def test_ninja_log_v6(self):
    return DiffTestBlueprint(
        trace=Path('build_log_v6.ninja_log'),
        query=_JOBS_QUERY,
        out=Csv(_JOBS))

  def test_ninja_log_v7(self):
    return DiffTestBlueprint(
        trace=Path('build_log_v7.ninja_log'),
        query=_JOBS_QUERY,
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
        query=_JOBS_QUERY,
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
        query=_JOBS_QUERY,
        out=Csv("""
          "worker","ts","dur","name"
          "Worker 1",0,100000000,"gen/foo.pb.h, gen/foo.pb.cc"
          "Worker 1",120000000,60000000,"obj/foo.o"
        """))

  # A log that went through a Windows checkout or editor has CRLF line
  # endings. The \r is not part of the header version nor of the command hash.
  def test_ninja_log_crlf(self):
    return DiffTestBlueprint(
        trace=Path('build_log_crlf.ninja_log'),
        query=_JOBS_QUERY,
        out=Csv("""
          "worker","ts","dur","name"
          "Worker 1",0,100000000,"obj/a.o"
          "Worker 2",0,120000000,"obj/b.o"
          "Worker 1",100000000,100000000,"obj/c.o"
        """))

  # Lines starting with # after the header are comments (e.g. the header of a
  # second log concatenated to the first): they are skipped, not counted as
  # lines that failed to parse.
  def test_ninja_log_comment(self):
    return DiffTestBlueprint(
        trace=Path('build_log_comment.ninja_log'),
        query="""
          select
            (select count(*) from track where type = 'ninja_worker')
              as workers,
            (select count(*) from slice
             join track on slice.track_id = track.id
             where track.type = 'ninja_worker') as slices,
            (select value from stats where name = 'ninja_parse_errors')
              as parse_errors
        """,
        out=Csv("""
          "workers","slices","parse_errors"
          2,3,0
        """))

  # The last line of a log that does not end with \n is still a job.
  def test_ninja_log_no_trailing_newline(self):
    return DiffTestBlueprint(
        trace=Path('build_log_no_trailing_newline.ninja_log'),
        query=_JOBS_QUERY,
        out=Csv("""
          "worker","ts","dur","name"
          "Worker 1",0,100000000,"obj/a.o"
          "Worker 2",0,120000000,"obj/b.o"
          "Worker 1",100000000,100000000,"obj/c.o"
        """))

  # When ninja recompacts a log it rewrites the last entry of every output in
  # the order of its hash map, so the first 20 lines here have end timestamps
  # that go backwards on every other line. They are not 10 builds: they are
  # dropped and counted, and only the two builds appended after them are
  # shown.
  def test_ninja_log_recompacted_prefix(self):
    return DiffTestBlueprint(
        trace=Path('build_log_recompacted_prefix.ninja_log'),
        query="""
          select
            ts, dur, name,
            (select value from stats
             where name = 'ninja_recompacted_entries_skipped') as skipped
          from slice
          where track_id in (select id from track where type = 'ninja_worker')
          order by ts, name
        """,
        out=Csv("""
          "ts","dur","name","skipped"
          0,40000000,"obj/r03.o",20
          0,90000000,"obj/r11.o",20
          90000000,60000000,"obj/r19.o",20
          155000000,55000000,"obj/r03.o",20
          210000000,40000000,"obj/r19.o",20
        """))

  # The same 20 lines with nothing appended after them. There is no build to
  # show instead, so they are all kept, at the timestamps they have in the
  # log rather than shifted one after the other.
  def test_ninja_log_recompacted(self):
    return DiffTestBlueprint(
        trace=Path('build_log_recompacted.ninja_log'),
        query="""
          select
            count(*) as slices,
            min(ts) as first_start,
            max(ts + dur) as last_end,
            (select value from stats
             where name = 'ninja_recompacted_entries_skipped') as skipped
          from slice
          where track_id in (select id from track where type = 'ninja_worker')
        """,
        out=Csv("""
          "slices","first_start","last_end","skipped"
          20,96000000,1774000000,0
        """))

  # A recompaction also scatters the outputs of a rule that has several of
  # them, which a build writes back-to-back. They are still one job: the three
  # outputs of the rule in this log are one slice, not three jobs running at
  # the same time on three workers.
  def test_ninja_log_recompacted_multi_output(self):
    return DiffTestBlueprint(
        trace=Path('build_log_recompacted_multi_output.ninja_log'),
        query="""
          select
            (select count(*) from slice
             join track on slice.track_id = track.id
             where track.type = 'ninja_worker') as slices,
            slice.ts, slice.dur, slice.name
          from slice
          where slice.name like 'gen/%'
        """,
        out=Csv("""
          "slices","ts","dur","name"
          21,200000000,500000000,"gen/foo.pb.h, gen/foo.pb.cc, gen/foo.pbzero.h"
        """))

  # The worker tracks are not threads of a made up process: the UI appends the
  # tid to the name of a thread track, which showed them as "Worker 1 1".
  def test_ninja_log_no_process_or_thread(self):
    return DiffTestBlueprint(
        trace=Path('build_log_v7.ninja_log'),
        query="""
          select
            (select count(*) from track where type = 'ninja_worker')
              as workers,
            (select count(*) from thread_track) as thread_tracks,
            (select count(*) from process where name = 'Build') as processes
        """,
        out=Csv("""
          "workers","thread_tracks","processes"
          6,0,0
        """))

  # One slice per build on a track of its own, from the moment ninja started
  # to the end of the last job of that build: the second build here starts
  # where the first one ends.
  def test_ninja_log_builds_track(self):
    return DiffTestBlueprint(
        trace=Path('build_log_two_builds.ninja_log'),
        query="""
          select track.name as track, slice.ts, slice.dur, slice.name
          from slice
          join track on slice.track_id = track.id
          where track.type = 'ninja_builds'
          order by slice.ts
        """,
        out=Csv("""
          "track","ts","dur","name"
          "Builds",0,200000000,"Build 1"
          "Builds",200000000,50000000,"Build 2"
        """))

  # The number of jobs running at each moment: obj/a.o and obj/b.o together,
  # then obj/c.o alone, then the one job of the second build.
  def test_ninja_log_active_jobs(self):
    return DiffTestBlueprint(
        trace=Path('build_log_two_builds.ninja_log'),
        query="""
          select counter_track.name as track, counter.ts, counter.value
          from counter
          join counter_track on counter.track_id = counter_track.id
          where counter_track.type = 'ninja_active_jobs'
          order by counter.ts
        """,
        out=Csv("""
          "track","ts","value"
          "Active jobs",0,2.000000
          "Active jobs",100000000,1.000000
          "Active jobs",200000000,1.000000
          "Active jobs",250000000,0.000000
        """))
