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
from google.protobuf import text_format


class Slices(TestSuite):

  def test_thread_slice(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE common.slices;

        SELECT name, ts, dur, depth, thread_name, tid, process_name, pid
        FROM thread_slice;
      """,
        out=Csv("""
        "name","ts","dur","depth","thread_name","tid","process_name","pid"
        "ThreadSlice",5,6,0,"Thread",5,"Process",3
      """))

  def test_process_slice(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE common.slices;

        SELECT name, ts, dur, depth, process_name, pid
        FROM process_slice;
      """,
        out=Csv("""
        "name","ts","dur","depth","process_name","pid"
        "ProcessSlice",3,4,0,"Process",3
      """))

  def test_slice_with_process_and_thread_info(self):
    return DiffTestBlueprint(
        trace=Path('trace.py'),
        query="""
        INCLUDE PERFETTO MODULE experimental.slices;

        SELECT name, ts, dur, depth, thread_name, tid, process_name, pid
        FROM experimental_slice_with_thread_and_process_info;
      """,
        out=Csv("""
        "name","ts","dur","depth","thread_name","tid","process_name","pid"
        "AsyncSlice",1,2,0,"[NULL]","[NULL]","[NULL]","[NULL]"
        "ProcessSlice",3,4,0,"[NULL]","[NULL]","Process",3
        "ThreadSlice",5,6,0,"Thread",5,"Process",3
      """))

  # Common functions
  def test_has_descendant_slice_with_name_true(self):
    return DiffTestBlueprint(
        # We need a trace with a large number of non-chrome slices, so that the
        # reliable range is affected by their filtering.
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE common.slices;

        SELECT
          HAS_DESCENDANT_SLICE_WITH_NAME(
            (SELECT id from slice where dur = 46046000),
            'SwapEndToPresentationCompositorFrame') AS has_descendant;
        """,
        out=Csv("""
        "has_descendant"
        1
        """))

  def test_has_descendant_slice_with_name_false(self):
    return DiffTestBlueprint(
        # We need a trace with a large number of non-chrome slices, so that the
        # reliable range is affected by their filtering.
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE common.slices;

        SELECT
          HAS_DESCENDANT_SLICE_WITH_NAME(
            (SELECT id from slice where dur = 11666000),
            'SwapEndToPresentationCompositorFrame') AS has_descendant;
        """,
        out=Csv("""
        "has_descendant"
        0
        """))

  def test_descendant_slice_null(self):
    return DiffTestBlueprint(
        # We need a trace with a large number of non-chrome slices, so that the
        # reliable range is affected by their filtering.
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE common.slices;

        SELECT
          DESCENDANT_SLICE_END(
            (SELECT id from slice where dur = 11666000),
            'SwapEndToPresentationCompositorFrame') AS end_ts;
        """,
        out=Csv("""
        "end_ts"
        "[NULL]"
        """))

  def test_descendant_slice(self):
    return DiffTestBlueprint(
        # We need a trace with a large number of non-chrome slices, so that the
        # reliable range is affected by their filtering.
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE common.slices;

        SELECT
          DESCENDANT_SLICE_END(
            (SELECT id from slice where dur = 46046000),
            'SwapEndToPresentationCompositorFrame') AS end_ts;
        """,
        out=Csv("""
        "end_ts"
        174797566610797
        """))

  def test_slice_flattened(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE experimental.flat_slices;

        SELECT e.name, e.ts, e.dur, e.depth
        FROM experimental_slice_flattened e
        JOIN thread_track ON e.track_id = thread_track.id
        JOIN thread USING(utid)
        WHERE thread.tid = 30944;
      """,
        out=Csv("""
        "name","ts","dur","depth"
        "ThreadControllerImpl::RunTask",174793737042797,3937000,0
        "ThreadControllerImpl::RunTask",174793741016797,5930000,0
        "ThreadControllerImpl::RunTask",174793747000797,47000,0
        "Receive mojo message",174793747047797,136000,1
        "ThreadControllerImpl::RunTask",174793747183797,17000,0
        "Looper.dispatch: android.os.Handler(Kx3@57873a8)",174793747546797,119000,0
        "ThreadControllerImpl::RunTask",174796099970797,186000,0
        "Looper.dispatch: jy3(null)",174800056530797,1368000,0
        "ThreadControllerImpl::RunTask",174800107962797,132000,0
      """))