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

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Zip(TestSuite):

  def test_perf_proto_sym(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query=Path('../simpleperf/stacks_test.sql'),
        out=Csv('''
        "name"
        "main,A"
        "main,A,B"
        "main,A,B,C"
        "main,A,B,C,D"
        "main,A,B,C,D,E"
        "main,A,B,C,E"
        "main,A,B,D"
        "main,A,B,D,E"
        "main,A,B,E"
        "main,A,C"
        "main,A,C,D"
        "main,A,C,D,E"
        "main,A,C,E"
        "main,A,D"
        "main,A,D,E"
        "main,A,E"
        "main,B"
        "main,B,C"
        "main,B,C,D"
        "main,B,C,D,E"
        "main,B,C,E"
        "main,B,D"
        "main,B,D,E"
        "main,B,E"
        "main,C"
        "main,C,D"
        "main,C,D,E"
        "main,C,E"
        "main,D"
        "main,D,E"
        "main,E"
        '''))

  def test_tokenization_order(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query='''
        SELECT *
        FROM __intrinsic_trace_file
        ORDER BY id
        ''',
        out=Csv('''
        "id","type","parent_id","name","size","trace_type"
        0,"__intrinsic_trace_file","[NULL]","[NULL]",94651,"zip"
        1,"__intrinsic_trace_file",0,"c.trace.pb",379760,"proto"
        2,"__intrinsic_trace_file",0,"b.simpleperf.data",554911,"perf"
        3,"__intrinsic_trace_file",0,"a.symbols.pb",186149,"symbols"
        '''))

  # Make sure the logcat timestamps are correctly converted to trace ts. All
  # logcat events in the trace were emitted while a perfetto trace collection
  # was active. Thus their timestamps should be between the min and max ts of
  # all track events.
  # The device where the trace was collected had a timezone setting of UTC+1
  def test_logcat_and_proto(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/logcat_and_proto.zip'),
        query='''
        WITH
          INTERVAL AS (
            SELECT
              (SELECT MIN(ts) FROM slice) AS min_ts,
              (SELECT MAX(ts) FROM slice) AS max_ts
          )
        SELECT COUNT(*) AS count
        FROM android_logs, INTERVAL
        WHERE ts BETWEEN min_ts AND max_ts;
        ''',
        out=Csv('''
        "count"
        58
        '''))
