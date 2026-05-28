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

  # android.display.video data source: top-level TracePacket.video_frame
  # (field 133). The codec_config packet carries display_name + codec_string;
  # subsequent au_data packets inherit both via the importer's per-uuid map.
  def test_video_frame_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          video_frame {
            display_id: 100
            display_name: "Built-in Screen"
            codec_string: "avc1.42c00b"
            codec: CODEC_H264
            codec_config: "\x00\x00\x00\x01sps\x00\x00\x00\x01pps"
          }
        }
        packet {
          timestamp: 1000016000
          video_frame {
            display_id: 100
            codec: CODEC_H264
            is_key_frame: true
            pts_us: 0
            frame_number: 0
            au_data: "\x00\x00\x00\x01\x65idr"
          }
        }
        packet {
          timestamp: 1000033000
          video_frame {
            display_id: 100
            codec: CODEC_H264
            pts_us: 16667
            frame_number: 1
            au_data: "\x00\x00\x00\x01\x61p1"
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.video_frames;
        SELECT ts, display_id, display_name, codec_string, frame_number,
               codec, is_key_frame, pts_us, is_config
        FROM android_video_frames
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","display_id","display_name","codec_string","frame_number","codec","is_key_frame","pts_us","is_config"
        1000000000,100,"Built-in Screen","avc1.42c00b",0,1,"[NULL]","[NULL]",1
        1000016000,100,"Built-in Screen","avc1.42c00b",0,1,1,0,"[NULL]"
        1000033000,100,"Built-in Screen","avc1.42c00b",1,1,0,16667,"[NULL]"
        """))

  # __intrinsic_video_frame_au_data(id) returns the encoded payload as a BLOB,
  # zero-copy from the original trace blob held in TraceStorage.
  def test_video_frame_au_data_blob(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          video_frame {
            display_id: 100
            display_name: "Display"
            codec: CODEC_H264
            codec_config: "CFG"
          }
        }
        packet {
          timestamp: 1000000001
          video_frame {
            display_id: 100
            codec: CODEC_H264
            is_key_frame: true
            au_data: "IDR-BYTES"
          }
        }
        """),
        query="""
        SELECT id,
               length(__intrinsic_video_frame_au_data(id)) AS byte_length,
               CAST(__intrinsic_video_frame_au_data(id) AS TEXT) AS bytes
        FROM __intrinsic_video_frames
        ORDER BY id;
        """,
        out=Csv("""
        "id","byte_length","bytes"
        0,3,"CFG"
        1,9,"IDR-BYTES"
        """))

  # Two displays in the same trace -> two streams, identified by display_id.
  def test_video_frame_multi_display(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          video_frame {
            display_id: 1
            display_name: "Front"
            codec: CODEC_H264
            codec_config: "C1"
          }
        }
        packet {
          timestamp: 1000000000
          video_frame {
            display_id: 2
            display_name: "Rear"
            codec: CODEC_HEVC
            codec_config: "C2"
          }
        }
        packet {
          timestamp: 1000016000
          video_frame {
            display_id: 1
            codec: CODEC_H264
            is_key_frame: true
            au_data: "F1"
          }
        }
        packet {
          timestamp: 1000016000
          video_frame {
            display_id: 2
            codec: CODEC_HEVC
            is_key_frame: true
            au_data: "R1"
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.video_frames;
        SELECT display_id, display_name, COUNT(*) AS rows
        FROM android_video_frames
        GROUP BY display_id
        ORDER BY display_id;
        """,
        out=Csv("""
        "display_id","display_name","rows"
        1,"Front",2
        2,"Rear",2
        """))

  # VideoFrameError: producer emits one packet per per-stream failure. The
  # importer routes each reason to its own kIndexed entry in the global
  # `stats` table keyed by display_id (ftrace per-cpu shape). Healthy
  # streams produce no entries.
  def test_video_frame_error(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000000000
          video_frame_error { display_id: 0 reason: SIZE_CAP_HIT }
        }
        packet {
          timestamp: 2000000000
          video_frame_error { display_id: 1 reason: CODEC_ERROR }
        }
        packet {
          timestamp: 3000000000
          video_frame_error { display_id: 2 reason: DISPLAY_GONE }
        }
        packet {
          timestamp: 4000000000
          video_frame_error { display_id: 3 reason: NO_ENCODER }
        }
        packet {
          timestamp: 5000000000
          video_frame_error { display_id: 4 reason: DISPLAY_NOT_FOUND }
        }
        packet {
          timestamp: 6000000000
          video_frame_error { display_id: 5 reason: ENCODER_SETUP_FAILED }
        }
        packet {
          timestamp: 7000000000
          video_frame_error { display_id: 6 reason: VIRTUAL_DISPLAY_FAILED }
        }
        # Second cap hit on display 0 -> counter increments to 2.
        packet {
          timestamp: 8000000000
          video_frame_error { display_id: 0 reason: SIZE_CAP_HIT }
        }
        """),
        query="""
        SELECT name, idx, value
        FROM stats
        WHERE name LIKE 'android_video_%'
        ORDER BY name, idx;
        """,
        out=Csv("""
        "name","idx","value"
        "android_video_codec_error",1,1
        "android_video_display_gone",2,1
        "android_video_display_not_found",4,1
        "android_video_encoder_setup_failed",5,1
        "android_video_no_encoder",3,1
        "android_video_size_cap_hit",0,2
        "android_video_virtual_display_failed",6,1
        """))
