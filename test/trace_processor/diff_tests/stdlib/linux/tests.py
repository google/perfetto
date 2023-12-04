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


class LinuxStdlib(TestSuite):

  def test_linux_cpu_idle_stats(self):
      return DiffTestBlueprint(
          trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event: {
                timestamp: 200000000000
                pid: 2
                cpu_frequency: {
                  state : 1704000
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200000000000
                pid: 2
                cpu_idle: {
                  state: 4294967295
                  cpu_id: 0
                }
              }
              event {
                timestamp: 200001000000
                pid: 2
                cpu_idle: {
                  state : 1
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200002000000
                pid  : 2
                cpu_idle: {
                  state : 4294967295
                  cpu_id: 0
                }
              }
              event {
                timestamp: 200003000000
                pid: 2
                cpu_idle: {
                  state : 1
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200004000000
                pid: 2
                cpu_idle: {
                  state : 4294967295
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200005000000
                pid: 2
                cpu_frequency: {
                  state: 300000
                  cpu_id: 0
                }
              }
            }
            trusted_uid: 9999
            trusted_packet_sequence_id: 2
          }
         """),
         query="""
         INCLUDE PERFETTO MODULE linux.cpu_idle;
         SELECT * FROM linux_cpu_idle_stats;
         """,
         out=Csv("""
         "cpu","state","count","dur","avg_dur","idle_percent"
         0,2,2,2000000,1000000,50.000013
         """))

