#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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


class CloneDurationMetrics(TestSuite):

  def test_clone_duration_by_buffer(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000
          trace_uuid {
            msb: 1
            lsb: 2
          }
        }
        packet {
          timestamp: 1000000
          service_event {
            clone_started: true
          }
        }
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
        """),
        query=Metric('clone_duration'),
        out=TextProto(r"""
        clone_duration {
          by_buffer {
            buffer: 0
            duration_ns: 3000000
          }
          by_buffer {
            buffer: 1
            duration_ns: 1000000
          }
          by_buffer {
            buffer: 2
            duration_ns: 2000000
          }
        }
        """))

  def test_clone_duration_by_buffer_missing_clone_started(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000
          trace_uuid {
            msb: 1
            lsb: 2
          }
        }
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
        """),
        query=Metric('clone_duration'),
        out=TextProto(r"""
        clone_duration {
        }
        """))

  def test_clone_duration_by_buffer_missing_buffer_cloned(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000
          trace_uuid {
            msb: 1
            lsb: 2
          }
        }
        packet {
          timestamp: 1000000
          service_event {
            clone_started: true
          }
        }
        """),
        query=Metric('clone_duration'),
        out=TextProto(r"""
        clone_duration {
        }
        """))
