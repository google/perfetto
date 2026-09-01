#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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


class AdrenoCmdbatch(TestSuite):

  def test_adreno_cmdbatch_retired_slice(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes { pid: 100 cmdline: "com.example.app" }
            threads { tid: 100 tgid: 100 }
          }
        }
        packet { trusted_packet_sequence_id: 1 ftrace_events {
          cpu: 0
          event {
            timestamp: 900000000
            pid: 100
            kgsl_adreno_cmdbatch_queued {
              id: 1
              timestamp: 42
              prio: 0
            }
          }
          event {
            timestamp: 1000000000
            pid: 100
            kgsl_adreno_cmdbatch_sync {
              id: 1
              timestamp: 42
              ticks: 19200000
              prio: 0
            }
          }
          event {
            timestamp: 3100000000
            pid: 100
            kgsl_adreno_cmdbatch_retired {
              id: 1
              timestamp: 42
              start: 38400000
              retire: 57600000
              prio: 0
            }
          }
        }}
        """),
        query="""
        SELECT
          slice.name as name,
          slice.ts as ts,
          slice.dur as dur,
          track.name as track_name
        FROM slice
        JOIN track ON slice.track_id = track.id
        WHERE track.type = 'adreno_gpu_cmdbatch'
        """,
        out=Csv("""
        "name","ts","dur","track_name"
        "GPU",2000000000,1000000000,"GPU com.example.app (Ctx=1, Prio=0)"
        """))

  def test_adreno_cmdbatch_cross_cpu(self):
    """Retired event on a different CPU bundle that arrives before sync."""
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes { pid: 100 cmdline: "com.example.app" }
            threads { tid: 100 tgid: 100 }
          }
        }
        packet { trusted_packet_sequence_id: 1 ftrace_events {
          cpu: 0
          event {
            timestamp: 900000000
            pid: 100
            kgsl_adreno_cmdbatch_queued {
              id: 1
              timestamp: 42
              prio: 0
            }
          }
        }}
        packet { trusted_packet_sequence_id: 1 ftrace_events {
          cpu: 3
          event {
            timestamp: 3100000000
            pid: 100
            kgsl_adreno_cmdbatch_retired {
              id: 1
              timestamp: 42
              start: 38400000
              retire: 57600000
              prio: 0
            }
          }
        }}
        packet { trusted_packet_sequence_id: 1 ftrace_events {
          cpu: 0
          event {
            timestamp: 1000000000
            pid: 100
            kgsl_adreno_cmdbatch_sync {
              id: 1
              timestamp: 42
              ticks: 19200000
              prio: 0
            }
          }
        }}
        """),
        query="""
        SELECT
          slice.name as name,
          slice.ts as ts,
          slice.dur as dur,
          track.name as track_name
        FROM slice
        JOIN track ON slice.track_id = track.id
        WHERE track.type = 'adreno_gpu_cmdbatch'
        """,
        out=Csv("""
        "name","ts","dur","track_name"
        "GPU",2000000000,1000000000,"GPU com.example.app (Ctx=1, Prio=0)"
        """))
