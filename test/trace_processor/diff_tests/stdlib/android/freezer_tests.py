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

from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto, Json, Path


class Freezer(TestSuite):

  def test_freezer_state_statsd_intervals(self):
    return DiffTestBlueprint(
        trace=Path('freezer_data.py'),
        query="""
            INCLUDE PERFETTO MODULE android.freezer;
            SELECT ts, dur, pid, process_name, freezer_state, time_unfrozen_millis, unfreeze_reason
            FROM android_freezer_state_statsd;
            """,
        out=Csv("""
            "ts","dur","pid","process_name","freezer_state","time_unfrozen_millis","unfreeze_reason"
            1000000000,4000000000,1234,"com.example.app","FREEZE_APP",0,"UFR_NONE"
            5000000000,0,1234,"com.example.app","UNFREEZE_APP",4,"UFR_ACTIVITY"
            """))

  def test_android_freezer_intervals(self):
    return DiffTestBlueprint(
        trace=Path('freezer_intervals.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.freezer;
        SELECT ts, dur, pid, state
        FROM _android_freezer_intervals
        ORDER BY ts, pid;
        """,
        out=Csv("""
        "ts","dur","pid","state"
        500,-1,400,"UNFROZEN"
        500,8500,500,"UNFROZEN"
        1000,2000,200,"UNFROZEN"
        3000,2000,200,"FROZEN"
        5000,1000,200,"UNFROZEN"
        6000,2000,200,"FROZEN"
        7000,-1,300,"FROZEN"
        9000,-1,500,"FROZEN"
        """))

  def test_android_freezer_intervals_bounded_by_event_duration(self):
    # Ring-buffer case: the freeze/unfreeze that started the state before each
    # process's first event was overwritten. The first event's duration bounds
    # the span: pid 600 was frozen for 2 s before its unfreeze, pid 700
    # unfrozen for 1 s before its freeze. pid 800's unfreeze carries no
    # duration, so its span still starts at trace start.
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          process_tree {
            processes { pid: 600 ppid: 1 cmdline: "proc600" }
            processes { pid: 700 ppid: 1 cmdline: "proc700" }
            processes { pid: 800 ppid: 1 cmdline: "proc800" }
          }
        }
        packet {
          timestamp: 1000000000
          track_descriptor {
            uuid: 2
            thread { pid: 100 tid: 100 thread_name: "system_server" }
          }
        }
        packet {
          timestamp: 1000000000
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            track_uuid: 2
            name: "trace_begin"
          }
        }
        packet {
          timestamp: 4000000000
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            track_uuid: 2
            name: "unfreeze"
            [com.android.internal.FrameworksBaseTrackEvent.freezer_event] {
              pid: 800
              uid: 10008
              unfreeze_reason: UFR_ACTIVITY
            }
          }
        }
        packet {
          timestamp: 5000000000
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            track_uuid: 2
            name: "unfreeze"
            [com.android.internal.FrameworksBaseTrackEvent.freezer_event] {
              pid: 600
              uid: 10006
              frozen_dur_ms: 2000
              unfreeze_reason: UFR_ACTIVITY
            }
          }
        }
        packet {
          timestamp: 6000000000
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            track_uuid: 2
            name: "freeze"
            [com.android.internal.FrameworksBaseTrackEvent.freezer_event] {
              pid: 700
              uid: 10007
              unfrozen_dur_ms: 1000
            }
          }
        }
        packet {
          timestamp: 7000000000
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            track_uuid: 2
            name: "end"
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.freezer;
        SELECT ts, dur, pid, state
        FROM _android_freezer_intervals
        ORDER BY pid, ts;
        """,
        out=Csv("""
        "ts","dur","pid","state"
        3000000000,2000000000,600,"FROZEN"
        5000000000,-1,600,"UNFROZEN"
        5000000000,1000000000,700,"UNFROZEN"
        6000000000,-1,700,"FROZEN"
        1000000000,3000000000,800,"FROZEN"
        4000000000,-1,800,"UNFROZEN"
        """))
