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


class Performance(TestSuite):
  # IRQ max runtime and count over 1ms
  def test_irq_runtime_metric(self):
    return DiffTestBlueprint(
        trace=Path('irq_runtime_metric.textproto'),
        query=Metric('android_irq_runtime'),
        out=Path('irq_runtime_metric.out'))

  # CPU frequency maximum & minimum limits change
  def test_cpu_frequency_limits(self):
    return DiffTestBlueprint(
        trace=Path('cpu_frequency_limits.textproto'),
        query="""
        SELECT
          ts,
          value,
          REPLACE(name, " Freq Limit", "") AS cpu
        FROM
          counter AS c
        LEFT JOIN
          counter_track AS t
          ON c.track_id = t.id
        WHERE
          name GLOB "* Freq Limit"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value","cpu"
        90000000,2800000.000000,"Cpu 6 Max"
        90000000,500000.000000,"Cpu 6 Min"
        100000000,1700000.000000,"Cpu 6 Max"
        100000000,500000.000000,"Cpu 6 Min"
        110000000,2800000.000000,"Cpu 6 Max"
        110000000,1400000.000000,"Cpu 6 Min"
        120000000,1500000.000000,"Cpu 6 Max"
        120000000,500000.000000,"Cpu 6 Min"
        120000000,1400000.000000,"Cpu 4 Max"
        120000000,600000.000000,"Cpu 4 Min"
        130000000,2200000.000000,"Cpu 4 Max"
        130000000,800000.000000,"Cpu 4 Min"
        """))

  # frame_timeline_metric collects App_Deadline_Missed metrics
  def test_frame_timeline_metric(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_metric.py'),
        query=Metric('android_frame_timeline_metric'),
        out=Path('frame_timeline_metric.out'))
