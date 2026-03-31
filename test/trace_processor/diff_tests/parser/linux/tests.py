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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Linux(TestSuite):

  def test_journald_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            clocks {
              clock_id: 1
              timestamp: 1000000000
            }
            clocks {
              clock_id: 6
              timestamp: 1000000000
            }
            primary_trace_clock: BUILTIN_CLOCK_BOOTTIME
          }
        }
        packet {
          timestamp: 1000000000
          journald_event {
            events {
              timestamp_us: 1000000
              pid: 42
              tid: 42
              prio: 6
              tag: "myapp"
              message: "hello world"
              comm: "myapp"
              uid: 1000
              systemd_unit: "myapp.service"
              hostname: "myhost"
              transport: "journal"
            }
          }
        }
        """),
        query="""
        SELECT ts, prio, tag, msg, uid, comm, systemd_unit, hostname, transport
        FROM journald_logs;
        """,
        out=Csv("""
        "ts","prio","tag","msg","uid","comm","systemd_unit","hostname","transport"
        1000000000,6,"myapp","hello world",1000,"myapp","myapp.service","myhost","journal"
        """))
