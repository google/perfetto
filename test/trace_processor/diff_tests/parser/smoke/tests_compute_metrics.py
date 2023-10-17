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


class SmokeComputeMetrics(TestSuite):
  # Contains smoke tests which test the most fundamentally important features
  # trace processor  Note: new tests here should only be added by the Perfetto
  # Compute CPU time metric testing several core tables.
  def test_thread_cpu_time_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          tid,
          pid,
          thread.name AS threadName,
          process.name AS processName,
          total_dur AS totalDur
        FROM
          thread
        LEFT JOIN process USING(upid)
        LEFT JOIN
          (SELECT upid, sum(dur) AS total_dur
            FROM sched JOIN thread USING(utid)
            WHERE dur != -1
            GROUP BY upid
          ) USING(upid)
        WHERE utid != 0
        ORDER BY total_dur DESC, pid, tid;
        """,
        out=Path('thread_cpu_time_example_android_trace_30s.out'))

  # Compute power proxy metric
  def test_proxy_power(self):
    return DiffTestBlueprint(
        trace=DataPath('cpu_counters.pb'),
        query="""
        SELECT RUN_METRIC('android/android_proxy_power.sql');

        DROP VIEW device;

        CREATE TABLE device (name STRING);

        INSERT INTO device VALUES ('walleye');

        SELECT
          tid,
          SUM(dur * COALESCE(power_ma, 0) / 1e9) AS power_mas
        FROM power_per_thread
        JOIN thread USING (utid)
        GROUP BY utid
        ORDER BY power_mas DESC
        LIMIT 10;
        """,
        out=Path('proxy_power.out'))
