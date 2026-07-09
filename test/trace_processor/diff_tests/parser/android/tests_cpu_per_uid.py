#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class AndroidCpuPerUid(TestSuite):

  def test_android_cpu_per_uid_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 10000
          cpu_per_uid_data {
            cluster_count: 3
            uid: 0
            uid: 1000
            uid: 1001
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
          }
        }
        packet {
          timestamp: 12000
          cpu_per_uid_data {
            uid: 0
            uid: 1000
            total_time_ms: 100
            total_time_ms: 0
            total_time_ms: 0
            total_time_ms: 2000
            total_time_ms: 200
            total_time_ms: 20
          }
        }
        """),
        query="""
        SELECT t.name, c.ts, c.value
        FROM counter_track t JOIN counter c ON t.id = c.track_id
        WHERE type = 'android_cpu_per_uid';
        """,
        out=Csv("""
        "name","ts","value"
        "CPU for UID 0 CL0",10000,1000000.000000
        "CPU for UID 0 CL0",12000,1000100.000000
        "CPU for UID 0 CL1",10000,1000000.000000
        "CPU for UID 0 CL1",12000,1000000.000000
        "CPU for UID 0 CL2",10000,1000000.000000
        "CPU for UID 0 CL2",12000,1000000.000000
        "CPU for UID 1000 CL0",10000,1000000.000000
        "CPU for UID 1000 CL0",12000,1002000.000000
        "CPU for UID 1000 CL1",10000,1000000.000000
        "CPU for UID 1000 CL1",12000,1000200.000000
        "CPU for UID 1000 CL2",10000,1000000.000000
        "CPU for UID 1000 CL2",12000,1000020.000000
        "CPU for UID 1001 CL0",10000,1000000.000000
        "CPU for UID 1001 CL0",12000,1000000.000000
        "CPU for UID 1001 CL1",10000,1000000.000000
        "CPU for UID 1001 CL1",12000,1000000.000000
        "CPU for UID 1001 CL2",10000,1000000.000000
        "CPU for UID 1001 CL2",12000,1000000.000000
        """))

  def test_android_cpu_per_uid_malformed(self):
    # Too few total_time_ms, then too many.
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 10000
          cpu_per_uid_data {
            cluster_count: 3
            uid: 0
            uid: 1000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
          }
        }
        packet {
          timestamp: 12000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
            total_time_ms: 1000000
          }
        }
        """),
        query="""
        SELECT t.name, c.ts, c.value
        FROM counter_track t JOIN counter c ON t.id = c.track_id
        WHERE type = 'android_cpu_per_uid';
        """,
        out=Csv("""
        "name","ts","value"
        "CPU for UID 0 CL0",10000,1000000.000000
        "CPU for UID 0 CL0",12000,2000000.000000
        "CPU for UID 0 CL1",10000,1000000.000000
        "CPU for UID 0 CL1",12000,2000000.000000
        "CPU for UID 0 CL2",10000,1000000.000000
        "CPU for UID 0 CL2",12000,2000000.000000
        "CPU for UID 1000 CL0",10000,1000000.000000
        "CPU for UID 1000 CL0",12000,1000000.000000
        "CPU for UID 1000 CL1",10000,1000000.000000
        "CPU for UID 1000 CL1",12000,1000000.000000
        """))
