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
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class SchedParser(TestSuite):
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

  def test_sched_cpu_util_cfs(self):
    return DiffTestBlueprint(
        trace=Path('sched_cpu_util_cfs.textproto'),
        query=Path('sched_cpu_util_cfs_test.sql'),
        out=Csv("""
        "name","ts","value"
        "Cpu 6 Util",10000,1.000000
        "Cpu 6 Cap",10000,1004.000000
        "Cpu 6 Nr Running",10000,0.000000
        "Cpu 7 Util",11000,1.000000
        "Cpu 7 Cap",11000,1007.000000
        "Cpu 7 Nr Running",11000,0.000000
        "Cpu 4 Util",12000,43.000000
        "Cpu 4 Cap",12000,760.000000
        "Cpu 4 Nr Running",12000,0.000000
        "Cpu 5 Util",13000,125.000000
        "Cpu 5 Cap",13000,757.000000
        "Cpu 5 Nr Running",13000,1.000000
        """))

  def test_sched_cpu_util_cfs_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('sched_cpu_util_cfs.textproto'),
        trace_modifier=TraceInjector(['ftrace_events'], {'machine_id': 1001}),
        query="""
        SELECT
          t.name,
          c.ts,
          c.value,
          m.raw_id as raw_machine_id
        FROM
          counter AS c
        JOIN
          counter_track AS t
          ON c.track_id = t.id
        JOIN machine as m on t.machine_id = m.id
        WHERE
          name GLOB "Cpu ? Cap" OR name GLOB "Cpu ? Util" OR name GLOB "Cpu ? Nr Running"
        ORDER BY ts;
        """,
        out=Csv("""
        "name","ts","value","raw_machine_id"
        "Cpu 6 Util",10000,1.000000,1001
        "Cpu 6 Cap",10000,1004.000000,1001
        "Cpu 6 Nr Running",10000,0.000000,1001
        "Cpu 7 Util",11000,1.000000,1001
        "Cpu 7 Cap",11000,1007.000000,1001
        "Cpu 7 Nr Running",11000,0.000000,1001
        "Cpu 4 Util",12000,43.000000,1001
        "Cpu 4 Cap",12000,760.000000,1001
        "Cpu 4 Nr Running",12000,0.000000,1001
        "Cpu 5 Util",13000,125.000000,1001
        "Cpu 5 Cap",13000,757.000000,1001
        "Cpu 5 Nr Running",13000,1.000000,1001
        """))
