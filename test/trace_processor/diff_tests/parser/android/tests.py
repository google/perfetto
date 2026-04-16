#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class AndroidParser(TestSuite):

  def test_android_system_property_counter(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          android_system_property {
            values {
              name: "debug.tracing.screen_state"
              value: "2"
            }
            values {
              name: "debug.tracing.device_state"
              value: "some_state_from_sysprops"
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 2000
              pid: 1
              print {
                buf: "C|1000|ScreenState|1\n"
              }
            }
            event {
              timestamp: 3000
              pid: 1
              print {
                buf: "N|1000|DeviceStateChanged|some_state_from_atrace\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT t.name, c.id, c.ts, c.value
        FROM counter_track t JOIN counter c ON t.id = c.track_id
        WHERE name = 'ScreenState';
        """,
        out=Csv("""
        "name","id","ts","value"
        "ScreenState",0,1000,2.000000
        "ScreenState",1,2000,1.000000
        """))

  def test_android_system_property_slice(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          android_system_property {
            values {
              name: "debug.tracing.screen_state"
              value: "2"
            }
            values {
              name: "debug.tracing.device_state"
              value: "some_state_from_sysprops"
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 2000
              pid: 1
              print {
                buf: "C|1000|ScreenState|1\n"
              }
            }
            event {
              timestamp: 3000
              pid: 1
              print {
                buf: "N|1000|DeviceStateChanged|some_state_from_atrace\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT t.name, s.id, s.ts, s.dur, s.name
        FROM track t JOIN slice s ON s.track_id = t.id
        WHERE t.name = 'DeviceStateChanged';
        """,
        out=Csv("""
        "name","id","ts","dur","name"
        "DeviceStateChanged",0,1000,0,"some_state_from_sysprops"
        "DeviceStateChanged",1,3000,0,"some_state_from_atrace"
        """))

  def test_binder_txn_sync_good(self):
    return DiffTestBlueprint(
        trace=Systrace(
            """          client-521390  [005] ..... 137012.464739: binder_command: cmd=0x40406300 BC_TRANSACTION
          client-521390  [005] ..... 137012.464741: binder_transaction: transaction=5149 dest_node=5143 dest_proc=521383 dest_thread=0 reply=0 flags=0x0 code=0x3
          server-521383  [004] ..... 137012.464771: binder_transaction_received: transaction=5149
          server-521383  [004] ..... 137012.464772: binder_return: cmd=0x80407202 BR_TRANSACTION
          server-521383  [004] ..... 137012.464815: binder_command: cmd=0x40086303 BC_FREE_BUFFER
          server-521383  [004] ..... 137012.464823: binder_command: cmd=0x40406301 BC_REPLY
          server-521383  [004] ..... 137012.464826: binder_transaction: transaction=5150 dest_node=0 dest_proc=521390 dest_thread=521390 reply=1 flags=0x20 code=0x3
          server-521383  [004] ..... 137012.464837: binder_return: cmd=0x7206 BR_TRANSACTION_COMPLETE
          client-521390  [005] ..... 137012.464847: binder_return: cmd=0x7206 BR_TRANSACTION_COMPLETE
          client-521390  [005] ..... 137012.464848: binder_transaction_received: transaction=5150
          client-521390  [005] ..... 137012.464849: binder_return: cmd=0x80407203 BR_REPLY
          """),
        query="""
      SELECT
        dur
      FROM slice
      ORDER BY dur;
      """,
        out=Csv("""
      "dur"
      55000
      107000
      """))

  def test_binder_txn_sync_bad_request(self):
    return DiffTestBlueprint(
        trace=Systrace(
            """          client-521349  [005] ..... 137004.281009: binder_command: cmd=0x40406300 BC_TRANSACTION
          client-521349  [005] ..... 137004.281010: binder_transaction: transaction=5135 dest_node=5129 dest_proc=521347 dest_thread=0 reply=0 flags=0x0 code=0x3
          client-521349  [005] ..... 137004.281410: binder_return: cmd=0x7211 BR_FAILED_REPLY
          """),
        query="""
      SELECT
        dur
      FROM slice
      ORDER BY dur;
      """,
        out=Csv("""
      "dur"
      400000
      """))

  def test_binder_txn_sync_bad_reply(self):
    return DiffTestBlueprint(
        trace=Systrace(
            """          client-521332  [007] ..... 136996.112660: binder_command: cmd=0x40406300 BC_TRANSACTION
          client-521332  [007] ..... 136996.112662: binder_transaction: transaction=5120 dest_node=5114 dest_proc=521330 dest_thread=0 reply=0 flags=0x0 code=0x3
          server-521330  [000] ..... 136996.112714: binder_transaction_received: transaction=5120
          server-521330  [000] ..... 136996.112715: binder_return: cmd=0x80407202 BR_TRANSACTION
          server-521330  [000] ..... 136996.112752: binder_command: cmd=0x40086303 BC_FREE_BUFFER
          server-521330  [000] ..... 136996.112758: binder_command: cmd=0x40406301 BC_REPLY
          server-521330  [000] ..... 136996.112760: binder_transaction: transaction=5121 dest_node=0 dest_proc=521332 dest_thread=521332 reply=1 flags=0x20 code=0x3
          server-521330  [000] ..... 136996.113163: binder_return: cmd=0x7206 BR_TRANSACTION_COMPLETE
          client-521332  [007] ..... 136996.113201: binder_return: cmd=0x7206 BR_TRANSACTION_COMPLETE
          client-521332  [007] ..... 136996.113201: binder_return: cmd=0x7211 BR_FAILED_REPLY
          """),
        query="""
      SELECT
        dur
      FROM slice
      ORDER BY dur;
      """,
        out=Csv("""
      "dur"
      46000
      539000
      """))

  def test_binder_txn_oneway_good(self):
    return DiffTestBlueprint(
        trace=Systrace(
            """          client-521406  [003] ..... 137020.679833: binder_command: cmd=0x40406300 BC_TRANSACTION
          client-521406  [003] ..... 137020.679834: binder_transaction: transaction=5161 dest_node=5155 dest_proc=521404 dest_thread=0 reply=0 flags=0x1 code=0x3
          client-521406  [003] ..... 137020.679843: binder_return: cmd=0x7206 BR_TRANSACTION_COMPLETE
          server-521404  [006] ..... 137020.679890: binder_transaction_received: transaction=5161
          server-521404  [006] ..... 137020.679890: binder_return: cmd=0x80407202 BR_TRANSACTION
          """),
        query="""
      SELECT
        dur
      FROM slice
      ORDER BY dur;
      """,
        out=Csv("""
      "dur"
      0
      0
      """))

  def test_android_user_list_dedup(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_pid: 1
          user_list {
            users {
              type: "android.os.usertype.full.SECONDARY"
              uid: 10
            }
          }
        }
        packet {
          trusted_pid: 2
          user_list {
            users {
              type: "android.os.usertype.full.SECONDARY"
              uid: 10
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.user_list;
        SELECT android_user_id, type FROM android_user_list
        ORDER BY android_user_id;
        """,
        out=Csv("""
        "android_user_id","type"
        10,"android.os.usertype.full.SECONDARY"
        """))

  # Tests when counter_tack.machine_id is not null.
  def test_android_system_property_counter_machine_id(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          android_system_property {
            values {
              name: "debug.tracing.screen_state"
              value: "2"
            }
            values {
              name: "debug.tracing.device_state"
              value: "some_state_from_sysprops"
            }
          }
          machine_id: 1001
        }
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 2000
              pid: 1
              print {
                buf: "C|1000|ScreenState|1\n"
              }
            }
            event {
              timestamp: 3000
              pid: 1
              print {
                buf: "N|1000|DeviceStateChanged|some_state_from_atrace\n"
              }
            }
          }
          machine_id: 1001
        }
        """),
        query="""
        SELECT t.name, c.id, c.ts, c.value
        FROM counter_track t JOIN counter c ON t.id = c.track_id
        WHERE name = 'ScreenState'
          AND t.machine_id IS NOT NULL;
        """,
        out=Csv("""
          "name","id","ts","value"
          "ScreenState",0,1000,2.000000
          "ScreenState",1,2000,1.000000
        """))

  def test_video_frame_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          timestamp_clock_id: 6
          video_frame {
            frame_number: 0
            track_name: "Front Camera"
            track_id: 1
          }
        }
        packet {
          timestamp: 1033333333
          timestamp_clock_id: 6
          video_frame {
            frame_number: 1
            track_name: "Front Camera"
            track_id: 1
          }
        }
        packet {
          timestamp: 1066666666
          timestamp_clock_id: 6
          video_frame {
            frame_number: 2
            track_name: "Rear Camera"
            track_id: 2
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.video_frames;
        SELECT ts, frame_number, track_name, track_id
        FROM android_video_frames
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","frame_number","track_name","track_id"
        1000000000,0,"Front Camera",1
        1033333333,1,"Front Camera",1
        1066666666,2,"Rear Camera",2
        """))

  def test_video_frame_no_track_fields(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          timestamp_clock_id: 6
          video_frame {
            frame_number: 0
          }
        }
        packet {
          timestamp: 1033333333
          timestamp_clock_id: 6
          video_frame {
            frame_number: 1
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.video_frames;
        SELECT ts, frame_number, track_name, track_id
        FROM android_video_frames
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","frame_number","track_name","track_id"
        1000000000,0,"[NULL]","[NULL]"
        1033333333,1,"[NULL]","[NULL]"
        """))

  def test_video_frame_trace_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          timestamp_clock_id: 6
          video_frame {
            frame_number: 0
            track_id: 1
          }
        }
        packet {
          timestamp: 2000000000
          timestamp_clock_id: 6
          video_frame {
            frame_number: 1
            track_id: 1
          }
        }
        """),
        query="""
        SELECT trace_start() AS s, trace_end() AS e, trace_dur() AS d;
        """,
        out=Csv("""
        "s","e","d"
        1000000000,2000000000,1000000000
        """))

  def test_video_frame_multi_stream_grouping(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          timestamp_clock_id: 6
          video_frame { frame_number: 0  track_name: "A"  track_id: 1 }
        }
        packet {
          timestamp: 1000000000
          timestamp_clock_id: 6
          video_frame { frame_number: 0  track_name: "B"  track_id: 2 }
        }
        packet {
          timestamp: 1033333333
          timestamp_clock_id: 6
          video_frame { frame_number: 1  track_name: "A"  track_id: 1 }
        }
        packet {
          timestamp: 1033333333
          timestamp_clock_id: 6
          video_frame { frame_number: 1  track_name: "B"  track_id: 2 }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.video_frames;
        SELECT
          COALESCE(track_id, 0) AS tid,
          COALESCE(track_name, 'Video Frames') AS tname,
          COUNT(*) AS cnt
        FROM android_video_frames
        GROUP BY tid
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","tname","cnt"
        1,"A",2
        2,"B",2
        """))
