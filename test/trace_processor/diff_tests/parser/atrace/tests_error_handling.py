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
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class AtraceErrorHandling(TestSuite):
  # Check error handling when parsing print events.
  def test_bad_print_textproto_list_slices(self):
    return DiffTestBlueprint(
        trace=Path('bad_print.textproto'),
        query="""
        SELECT ts, dur, name
        FROM slice;
        """,
        out=Csv("""
        "ts","dur","name"
        74662603048,2,"valid_print"
        """))

  def test_bad_print_systrace_list_slices(self):
    return DiffTestBlueprint(
        trace=Path('bad_print.systrace'),
        query="""
        SELECT ts, dur, name
        FROM slice;
        """,
        out=Csv("""
        "ts","dur","name"
        10852771242000,3000,"some event"
        """))

  def test_instant_atrace_instant_with_thread(self):
    return DiffTestBlueprint(
        trace=Path('instant_atrace.py'),
        query="""
        SELECT 
            thread.name AS thread_name, 
            instant.name AS track_name, 
            instant.ts
        FROM slice instant
        JOIN thread_track ON instant.track_id = thread_track.id
        JOIN thread USING (utid)
        WHERE dur = 0;
        """,
        out=Csv("""
        "thread_name","track_name","ts"
        "t2","t2_event",51
        "t1","t1_event",53
        """))

  def test_instant_async_atrace_instant_async(self):
    return DiffTestBlueprint(
        trace=Path('instant_async_atrace.py'),
        query="""
        SELECT
          process.name AS process_name,
          process_track.name AS track_name,
          instant.name AS instant_name,
          ts
        FROM slice instant
        JOIN process_track ON instant.track_id = process_track.id
        JOIN process USING (upid)
        WHERE dur = 0;
        """,
        out=Csv("""
        "process_name","track_name","instant_name","ts"
        "p2","track_p2","ev1",51
        "p1","track_p1","ev2",53
        """))
