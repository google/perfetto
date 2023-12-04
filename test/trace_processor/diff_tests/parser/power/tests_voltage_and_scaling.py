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


class PowerVoltageAndScaling(TestSuite):

  def test_dvfs_metric(self):
    return DiffTestBlueprint(
        trace=Path('dvfs_metric.textproto'),
        query=Metric('android_dvfs'),
        out=Path('dvfs_metric.out'))

  def test_wakesource_wakesource(self):
    return DiffTestBlueprint(
        trace=Path('wakesource.textproto'),
        query="""
        SELECT ts, dur, slice.name
        FROM slice
        JOIN track ON slice.track_id = track.id
        WHERE track.name GLOB 'Wakelock*'
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","name"
        34298714043271,7872467,"Wakelock(s2mpw02-power-keys)"
        34298721846504,42732654,"Wakelock(event0)"
        34298721915739,16,"Wakelock(s2mpw02-power-keys)"
        34298764569658,14538,"Wakelock(eventpoll)"
        """))

  def test_suspend_resume(self):
    return DiffTestBlueprint(
        trace=Path('suspend_resume.textproto'),
        query="""
        SELECT
          s.ts,
          s.dur,
          s.name AS action
        FROM
          slice AS s
        JOIN
          track AS t
          ON s.track_id = t.id
        WHERE
          t.name = 'Suspend/Resume Latency'
        ORDER BY s.ts;
        """,
        out=Csv("""
        "ts","dur","action"
        10000,5000,"suspend_enter(3)"
        15000,5000,"suspend_enter(3)"
        30000,10000,"CPU(0)"
        50000,10000,"timekeeping_freeze(0)"
        """))

  def test_suspend_period(self):
    return DiffTestBlueprint(
        trace=Path('suspend_period.textproto'),
        query=Metric('android_batt'),
        out=TextProto(r"""
        android_batt {
          battery_aggregates {
            sleep_ns: 20000
          }
          suspend_period {
            timestamp_ns: 30000
            duration_ns: 10000
          }
          suspend_period {
            timestamp_ns: 50000
            duration_ns: 10000
          }
        }
        """))
