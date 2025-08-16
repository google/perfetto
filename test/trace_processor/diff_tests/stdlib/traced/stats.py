#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

BASE_TRACE = r"""
        packet {
          timestamp: 500000
          trace_uuid {
            msb: 1
            lsb: 2
          }
        }"""

CLONE_TRIGGERED = r"""
        packet {
          timestamp: 500000
          clone_snapshot_trigger {
            trigger_name: "trigger_name"
          }
        }
        """

CLONE_STARTED = r"""
        packet {
          timestamp: 1000000
          service_event {
            clone_started: true
          }
        }
        """

CLONE_DONE = r"""
        packet {
          timestamp: 2000000
          service_event {
            buffer_cloned: 1
          }
        }
        packet {
          timestamp: 3000000
          service_event {
            buffer_cloned: 2
          }
        }
        packet {
          timestamp: 4000000
          service_event {
            buffer_cloned: 0
          }
        }
        """


class TracedStats(TestSuite):

  def test_clone_flush_latency(self):
    return DiffTestBlueprint(
        trace=TextProto(BASE_TRACE + CLONE_STARTED + CLONE_DONE),
        query=r"""
        INCLUDE PERFETTO MODULE traced.stats;

        SELECT * FROM traced_clone_flush_latency;
        """,
        out=Csv("""
        "buffer_id","duration_ns"
        0,3000000
        1,1000000
        2,2000000
        """))

  def test_clone_flush_latency_missing_clone_started(self):
    return DiffTestBlueprint(
        trace=TextProto(BASE_TRACE + CLONE_DONE),
        query=r"""
        INCLUDE PERFETTO MODULE traced.stats;

        SELECT * FROM traced_clone_flush_latency;
        """,
        out=Csv("""
        "buffer_id","duration_ns"
        """))

  def test_clone_flush_latency_missing_buffer_cloned(self):
    return DiffTestBlueprint(
        trace=TextProto(BASE_TRACE + CLONE_STARTED),
        query=r"""
        INCLUDE PERFETTO MODULE traced.stats;

        SELECT * FROM traced_clone_flush_latency;
        """,
        out=Csv("""
        "buffer_id","duration_ns"
        """))

  def test_trigger_clone_flush_latency(self):
    return DiffTestBlueprint(
        trace=TextProto(BASE_TRACE + CLONE_TRIGGERED + CLONE_DONE),
        query=r"""
        INCLUDE PERFETTO MODULE traced.stats;

        SELECT * FROM traced_trigger_clone_flush_latency;
        """,
        out=Csv("""
        "buffer_id","duration_ns"
        0,3500000
        1,1500000
        2,2500000
        """))

  def test_trigger_clone_flush_latency_missing_clone_started(self):
    return DiffTestBlueprint(
        trace=TextProto(BASE_TRACE + CLONE_DONE),
        query=r"""
        INCLUDE PERFETTO MODULE traced.stats;

        SELECT * FROM traced_trigger_clone_flush_latency;
        """,
        out=Csv("""
        "buffer_id","duration_ns"
        """))

  def test_trigger_clone_flush_latency_missing_buffer_cloned(self):
    return DiffTestBlueprint(
        trace=TextProto(BASE_TRACE + CLONE_TRIGGERED),
        query=r"""
        INCLUDE PERFETTO MODULE traced.stats;

        SELECT * FROM traced_trigger_clone_flush_latency;
        """,
        out=Csv("""
        "buffer_id","duration_ns"
        """))
