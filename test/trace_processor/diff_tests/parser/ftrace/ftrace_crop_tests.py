#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class FtraceCrop(TestSuite):

  # Expect the first begin event on cpu1 gets suppressed as it is below the
  # maximum of previous_bundle_end_timestamps.
  def test_crop_atrace_slice(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 1
          previous_bundle_end_timestamp: 1000
          event {
            timestamp: 1500
            pid: 42
            print { buf: "B|42|FilteredOut\n" }
          }
          event {
            timestamp: 2700
            pid: 42
            print { buf: "E|42\n" }
          }
        }}
        packet { ftrace_events {
          cpu: 0
          previous_bundle_end_timestamp: 2000
          event {
            timestamp: 2200
            pid: 42
            print { buf: "B|42|Kept\n" }
          }
        }}
        """),
        query="""
        select
          ts,
          rtrim(extract_arg(raw.arg_set_id, "buf"), char(0x0a)) as raw_print,
          slice.dur as slice_dur,
          slice.name as slice_name
        from raw left join slice using (ts)
        """,
        out=Csv("""
        "ts","raw_print","slice_dur","slice_name"
        1500,"B|42|FilteredOut","[NULL]","[NULL]"
        2200,"B|42|Kept",500,"Kept"
        2700,"E|42","[NULL]","[NULL]"
        """))

  # As test_crop_atrace_slice, with the older "last_read_event_timestamp" field
  def test_crop_atrace_slice_legacy_field(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 1
          last_read_event_timestamp: 1000
          event {
            timestamp: 1500
            pid: 42
            print { buf: "B|42|FilteredOut\n" }
          }
          event {
            timestamp: 2700
            pid: 42
            print { buf: "E|42\n" }
          }
        }}
        packet { ftrace_events {
          cpu: 0
          last_read_event_timestamp: 2000
          event {
            timestamp: 2200
            pid: 42
            print { buf: "B|42|Kept\n" }
          }
        }}
        """),
        query="""
        select
          ts,
          rtrim(extract_arg(raw.arg_set_id, "buf"), char(0x0a)) as raw_print,
          slice.dur as slice_dur,
          slice.name as slice_name
        from raw left join slice using (ts)
        """,
        out=Csv("""
        "ts","raw_print","slice_dur","slice_name"
        1500,"B|42|FilteredOut","[NULL]","[NULL]"
        2200,"B|42|Kept",500,"Kept"
        2700,"E|42","[NULL]","[NULL]"
        """))

  # First compact_switch per cpu doesn't generate any events, successive
  # switches generate a |raw| entry, but no scheduling slices until past all
  # previous_bundle_end_timestamps.
  def test_crop_compact_sched_switch(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            previous_bundle_end_timestamp: 1000
            cpu: 3
            compact_sched {
              intern_table: "zero:3"
              intern_table: "one:3"
              switch_timestamp: 1100
              switch_timestamp: 100
              switch_next_pid: 50
              switch_next_pid: 51
              switch_prev_state: 0
              switch_prev_state: 0
              switch_next_prio: 120
              switch_next_prio: 120
              switch_next_comm_index: 0
              switch_next_comm_index: 1
            }
          }
        }
        packet {
          ftrace_events {
            previous_bundle_end_timestamp: 0
            cpu: 6
            compact_sched {
              intern_table: "zero:6"
              intern_table: "one:6"
              intern_table: "two:6"
              intern_table: "three:6"
              switch_timestamp: 500
              switch_timestamp: 100
              switch_timestamp: 500
              switch_timestamp: 100
              switch_next_pid: 40
              switch_next_pid: 41
              switch_next_pid: 42
              switch_next_pid: 43
              switch_prev_state: 0
              switch_prev_state: 0
              switch_prev_state: 0
              switch_prev_state: 0
              switch_next_prio: 120
              switch_next_prio: 120
              switch_next_prio: 120
              switch_next_prio: 120
              switch_next_comm_index: 0
              switch_next_comm_index: 1
              switch_next_comm_index: 2
              switch_next_comm_index: 3
            }
          }
        }
        """),
        query="""
        select cpu, ts, dur, tid, thread.name
        from sched join thread using (utid)
        order by cpu asc, ts asc
        """,
        out=Csv("""
        "cpu","ts","dur","tid","name"
        3,1200,-1,51,"one:3"
        6,1100,100,42,"two:6"
        6,1200,-1,43,"three:6"
        """))
