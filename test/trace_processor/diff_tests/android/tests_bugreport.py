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


class AndroidBugreport(TestSuite):

  def test_android_bugreport_logs(self):
    return DiffTestBlueprint(
        trace=DataPath('bugreport-crosshatch-SPB5.zip'),
        query="""
        WITH
        initial AS (SELECT
            (SELECT count(*) FROM android_logs) AS cnt,
            ts, prio, tag, msg FROM android_logs
          ORDER BY ts ASC
          LIMIT 100
        ),
        latest AS (SELECT
            (SELECT count(*) FROM android_logs) AS cnt,
            ts, prio, tag, msg FROM android_logs
          ORDER BY ts DESC
          LIMIT 100
        )
        SELECT * FROM initial UNION ALL SELECT * FROM latest;
        """,
        out=Path('android_bugreport_logs_test.out'))

  def test_android_bugreport_dumpstate(self):
    return DiffTestBlueprint(
        trace=DataPath('bugreport-crosshatch-SPB5.zip'),
        query="""
        SELECT section, service, count(line) AS linecount FROM android_dumpstate
        GROUP BY section, service;
        """,
        out=Path('android_bugreport_dumpstate_test.out'))

  def test_android_bugreport_dumpsys(self):
    return DiffTestBlueprint(
        trace=DataPath('bugreport-crosshatch-SPB5.zip'),
        query="""
        SELECT section, service, line FROM android_dumpstate
        WHERE service = 'color_display';
        """,
        out=Path('android_bugreport_dumpsys_test.out'))
