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

  def test_android_framework_track_event_process(self):
    return DiffTestBlueprint(
        trace=Path('android_framework_track_event.textproto'),
        query="""
        SELECT
          p.pid,
          p.name,
          p.uid,
          t.start_seq_id,
          t.package_uid,
          t.caller_uid,
          t.defining_uid,
          t.fw_start_ts,
          t.fw_end_ts
        FROM __intrinsic_android_track_event_process t
        JOIN process p USING (upid);
        """,
        out=Csv("""
          "pid","name","uid","start_seq_id","package_uid","caller_uid","defining_uid","fw_start_ts","fw_end_ts"
          100,"com.example.app",10001,42,10002,10003,10004,2000,5000
        """))

  def test_android_framework_track_event_process_died(self):
    return DiffTestBlueprint(
        trace=Path('android_framework_track_event_process_died.textproto'),
        query="""
        SELECT
          p.pid,
          t.start_seq_id,
          t.fw_start_ts,
          t.fw_end_ts,
          p.end_ts,
          t.exit_reason,
          t.exit_sub_reason
        FROM __intrinsic_android_track_event_process t
        JOIN process p USING (upid)
        ORDER BY p.pid, t.upid;
        """,
        out=Csv("""
          "pid","start_seq_id","fw_start_ts","fw_end_ts","end_ts","exit_reason","exit_sub_reason"
          100,1,1000000000,2000000000,2000000000,"APP_EXIT_REASON_CRASH","APP_EXIT_SUBREASON_TOO_MANY_CACHED"
          100,3,3000000000,"[NULL]","[NULL]","[NULL]","[NULL]"
          200,2,1000000000,3000000000,3000000000,"APP_EXIT_REASON_ANR","[NULL]"
          300,4,1000000000,"[NULL]","[NULL]","APP_EXIT_REASON_ANR","[NULL]"
          400,5,"[NULL]",2000000000,2000000000,"APP_EXIT_REASON_LOW_MEMORY","[NULL]"
          500,6,1000000000,"[NULL]","[NULL]","[NULL]","[NULL]"
          600,"[NULL]","[NULL]","[NULL]","[NULL]","APP_EXIT_REASON_SIGNALED","[NULL]"
          700,7,"[NULL]",2000000000,2000000000,"APP_EXIT_REASON_FREEZER","[NULL]"
          800,9,1000000000,"[NULL]","[NULL]","[NULL]","[NULL]"
          1100,10,1000000000,4000000000,2000000000,"[NULL]","[NULL]"
          1100,11,3000000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        """))

  def test_android_framework_track_event_enum(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor { uuid: 2 thread { pid: 100 tid: 100 } }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 100
          extension_descriptor {
            extension_set {
              file {
                package: "com.android.internal"
                name: "fbte_enums.proto"
                enum_type {
                  name: "TriggerType"
                  value { name: "TRIGGER_TYPE_UNKNOWN" number: 0 }
                  value { name: "TRIGGER_TYPE_JOB" number: 4 }
                }
                enum_type {
                  name: "HostingTypeId"
                  value { name: "HOSTING_TYPE_UNKNOWN" number: 0 }
                  value { name: "HOSTING_TYPE_SERVICE" number: 11 }
                }
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            track_uuid: 2
            type: TYPE_INSTANT
            [com.android.internal.FrameworksBaseTrackEvent.process_start_event] {
              pid: 100
              trigger_type: TRIGGER_TYPE_JOB
              hosting_type: HOSTING_TYPE_SERVICE
            }
          }
        }
        """),
        query="""
        SELECT trigger_type, hosting_type
        FROM __intrinsic_android_track_event_process;
        """,
        out=Csv("""
          "trigger_type","hosting_type"
          "TRIGGER_TYPE_JOB","HOSTING_TYPE_SERVICE"
        """))

  def test_android_process_state(self):
    return DiffTestBlueprint(
        trace=Path('android_process_state.textproto'),
        query="""
        SELECT
          t.ts,
          p.pid,
          t.utid,
          t.proc_state,
          t.oom_score,
          t.capability_flags,
          t.process_group,
          t.reason,
          t.seq_id,
          t.is_initial
        FROM __intrinsic_android_process_state t
        JOIN process p USING (upid)
        ORDER BY p.pid, t.ts, t.reason;
        """,
        out=Csv("""
          "ts","pid","utid","proc_state","oom_score","capability_flags","process_group","reason","seq_id","is_initial"
          "[NULL]",100,"[NULL]","PROCESS_STATE_TOP",0,1,"PROCESS_GROUP_TOP_APP","[NULL]","[NULL]",1
          "[NULL]",200,"[NULL]","PROCESS_STATE_TOP",0,1,"PROCESS_GROUP_TOP_APP","[NULL]","[NULL]",1
          2000,200,3,"PROCESS_STATE_IMPORTANT_FOREGROUND",200,0,"PROCESS_GROUP_FOREGROUND","OOM_ADJ_REASON_START_RECEIVER",10,0
          4000,200,3,"PROCESS_STATE_CACHED_ACTIVITY",900,0,"PROCESS_GROUP_BACKGROUND","OOM_ADJ_REASON_BIND_SERVICE",11,0
          "[NULL]",300,"[NULL]","PROCESS_STATE_PERSISTENT",-1000,1,"PROCESS_GROUP_SYSTEM","[NULL]","[NULL]",1
          "[NULL]",400,"[NULL]","PROCESS_STATE_FOREGROUND_SERVICE",0,0,"PROCESS_GROUP_UNKNOWN","[NULL]","[NULL]",1
          "[NULL]",500,"[NULL]","PROCESS_STATE_TOP",0,1,"PROCESS_GROUP_TOP_APP","[NULL]","[NULL]",1
          2000,500,3,"PROCESS_STATE_IMPORTANT_FOREGROUND",300,0,"PROCESS_GROUP_BACKGROUND","OOM_ADJ_REASON_BIND_SERVICE",21,0
          2000,500,3,"PROCESS_STATE_BOUND_FOREGROUND_SERVICE",250,0,"PROCESS_GROUP_FOREGROUND","OOM_ADJ_REASON_START_RECEIVER",20,0
        """))

  def test_android_freezer_state(self):
    return DiffTestBlueprint(
        trace=Path('android_process_state.textproto'),
        query="""
        SELECT
          t.ts,
          p.pid,
          t.unfrozen_dur_ms,
          t.frozen_dur_ms,
          t.unfreeze_reason,
          t.is_initial
        FROM __intrinsic_android_freezer_state t
        JOIN process p USING (upid)
        ORDER BY p.pid, t.ts;
        """,
        out=Csv("""
          "ts","pid","unfrozen_dur_ms","frozen_dur_ms","unfreeze_reason","is_initial"
          "[NULL]",100,"[NULL]","[NULL]","UFR_ACTIVITY",1
          3000,200,100,300,"UFR_BIND_SERVICE",0
          "[NULL]",600,"[NULL]","[NULL]","UFR_PING",1
          "[NULL]",700,"[NULL]","[NULL]","UFR_NONE",1
        """))

  def test_android_process_state_metadata(self):
    return DiffTestBlueprint(
        trace=Path('android_process_state_metadata.textproto'),
        query="""
        SELECT
          p.pid,
          p.name,
          p.uid,
          p.android_user_id,
          t.start_seq_id,
          t.fw_start_ts,
          t.fw_end_ts,
          p.end_ts
        FROM process p
        LEFT JOIN __intrinsic_android_track_event_process t
          ON t.upid = p.upid
        WHERE p.pid > 0
        ORDER BY p.pid;
        """,
        out=Csv("""
          "pid","name","uid","android_user_id","start_seq_id","fw_start_ts","fw_end_ts","end_ts"
          100,"com.example.appa",10001,0,1,1000000000,"[NULL]","[NULL]"
          200,"com.example.dumps_only",20001,0,10,"[NULL]","[NULL]","[NULL]"
          300,"system_server",1000,0,"[NULL]","[NULL]","[NULL]","[NULL]"
          400,"com.example.pre_existing",40001,0,5,"[NULL]",3000000000,3000000000
          500,"com.example.started_late",50001,0,6,2000000000,"[NULL]","[NULL]"
        """))

  def test_android_process_state_metadata_freezer(self):
    return DiffTestBlueprint(
        trace=Path('android_process_state_metadata.textproto'),
        query="""
        SELECT t.ts, p.pid, t.unfreeze_reason, t.is_initial
        FROM __intrinsic_android_freezer_state t
        JOIN process p USING (upid)
        ORDER BY p.pid, t.ts;
        """,
        out=Csv("""
          "ts","pid","unfreeze_reason","is_initial"
          "[NULL]",200,"UFR_PING",1
          2500000000,400,"UFR_BINDER_TXNS",0
        """))

  def test_android_process_state_metadata_state(self):
    return DiffTestBlueprint(
        trace=Path('android_process_state_metadata.textproto'),
        query="""
        SELECT
          t.ts,
          p.pid,
          t.proc_state,
          t.oom_score,
          t.capability_flags,
          t.process_group,
          t.reason,
          t.seq_id,
          t.is_initial
        FROM __intrinsic_android_process_state t
        JOIN process p USING (upid)
        ORDER BY p.pid, t.ts;
        """,
        out=Csv("""
          "ts","pid","proc_state","oom_score","capability_flags","process_group","reason","seq_id","is_initial"
          "[NULL]",100,"PROCESS_STATE_TOP",100,0,"PROCESS_GROUP_TOP_APP","[NULL]","[NULL]",1
          "[NULL]",200,"PROCESS_STATE_PERSISTENT",-1000,0,"PROCESS_GROUP_SYSTEM","[NULL]","[NULL]",1
          "[NULL]",300,"PROCESS_STATE_PERSISTENT",-1000,0,"PROCESS_GROUP_UNKNOWN","[NULL]","[NULL]",1
          "[NULL]",400,"PROCESS_STATE_TOP",0,1,"PROCESS_GROUP_TOP_APP","[NULL]","[NULL]",1
          2000000000,400,"PROCESS_STATE_IMPORTANT_FOREGROUND",200,0,"PROCESS_GROUP_FOREGROUND","OOM_ADJ_REASON_START_RECEIVER",7,0
          "[NULL]",500,"PROCESS_STATE_TOP",100,0,"PROCESS_GROUP_TOP_APP","[NULL]","[NULL]",1
        """))

  def test_android_process_state_died(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_START
            record { pid: 100 uid: 10050 process_name: "com.example.a" start_seq_id: 1 }
            record { pid: 200 uid: 10060 process_name: "com.example.b" start_seq_id: 2 }
          }
        }
        # pid 100 has a state change before dying: initial state comes from the
        # earlier state change, not process_state_died.
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            type: TYPE_INSTANT
            name: "process_state_changed"
            [com.android.internal.FrameworksBaseTrackEvent.process_state_changed_event] {
              pid: 100
              uid: 10050
              prev_proc_state: PROCESS_STATE_TOP
              cur_proc_state: PROCESS_STATE_CACHED_EMPTY
              prev_oom_score: 0
              cur_oom_score: 900
              prev_capability_flags: 1
              cur_capability_flags: 0
              reason: OOM_ADJ_REASON_ACTIVITY
              seq_id: 5
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          track_event {
            type: TYPE_INSTANT
            name: "process_state_died"
            [com.android.internal.FrameworksBaseTrackEvent.process_state_died_event] {
              pid: 100
              uid: 10050
              prev_proc_state: PROCESS_STATE_CACHED_EMPTY
              prev_oom_score: 900
              prev_capability_flags: 0
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3001
          track_event {
            type: TYPE_INSTANT
            name: "binder_died"
            [com.android.internal.FrameworksBaseTrackEvent.binder_died_event] {
              pid: 100
              uid: 10050
              start_seq_id: 1
            }
          }
        }
        # pid 200 never changed state before dying and is not in the END dump:
        # initial state comes from process_state_died.
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 4000
          track_event {
            type: TYPE_INSTANT
            name: "process_state_died"
            [com.android.internal.FrameworksBaseTrackEvent.process_state_died_event] {
              pid: 200
              uid: 10060
              prev_proc_state: PROCESS_STATE_FOREGROUND_SERVICE
              prev_oom_score: 200
              prev_capability_flags: 4
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 4001
          track_event {
            type: TYPE_INSTANT
            name: "binder_died"
            [com.android.internal.FrameworksBaseTrackEvent.binder_died_event] {
              pid: 200
              uid: 10060
              start_seq_id: 2
            }
          }
        }
        """),
        query="""
        SELECT
          t.ts,
          p.pid,
          t.proc_state,
          t.oom_score,
          t.capability_flags,
          t.is_initial
        FROM __intrinsic_android_process_state t
        JOIN process p USING (upid)
        ORDER BY p.pid, t.ts;
        """,
        out=Csv("""
          "ts","pid","proc_state","oom_score","capability_flags","is_initial"
          "[NULL]",100,"PROCESS_STATE_TOP",0,1,1
          2000,100,"PROCESS_STATE_CACHED_EMPTY",900,0,0
          "[NULL]",200,"PROCESS_STATE_FOREGROUND_SERVICE",200,4,1
        """))

  def test_android_process_state_graph_and_triggers(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_START
            record { pid: 100 uid: 10001 process_name: "com.example.top" start_seq_id: 1 }
            record { pid: 200 uid: 10002 process_name: "com.example.worker" start_seq_id: 2 }
            record { pid: 300 uid: 10003 process_name: "com.example.provider" start_seq_id: 3 }
          }
        }
        # Pass seq_id = 10: broadcast dispatch + service binding triggers worker promotion
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1900
          track_event {
            type: TYPE_INSTANT
            name: "broadcast_dispatch_started"
            [com.android.internal.FrameworksBaseTrackEvent.broadcast_event] {
              action_name: "android.intent.action.SYNC_NOW"
              receiver_pid: 200
              receiver_uid: 10002
              sender_pid: 100
              sender_uid: 10001
              broadcast_type: 18
              receiver_component_name: "com.example.worker/.SyncReceiver"
              seq_id: 10
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1950
          track_event {
            type: TYPE_INSTANT
            name: "service_binding"
            [com.android.internal.FrameworksBaseTrackEvent.service_state_changed_event] {
              component_name: "com.example.worker/.SyncService"
              service_id: 201
              pid: 200
              uid: 10002
              caller_pid: 100
              caller_uid: 10001
              intent_bind_id: 401
              bind_id: 501
              bind_flags_32: 1
              seq_id: 10
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            type: TYPE_INSTANT
            name: "oom_adjuster_pass"
            [com.android.internal.FrameworksBaseTrackEvent.oom_adjuster_pass_event] {
              seq_id: 10
              reason: OOM_ADJ_REASON_BIND_SERVICE
              is_full_update: false
              top_pid: 100
              top_proc_state: PROCESS_STATE_TOP
              target_pid: 200
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            type: TYPE_INSTANT
            name: "process_state_changed"
            [com.android.internal.FrameworksBaseTrackEvent.process_state_changed_event] {
              pid: 200
              uid: 10002
              prev_proc_state: PROCESS_STATE_CACHED_EMPTY
              cur_proc_state: PROCESS_STATE_BOUND_TOP
              prev_oom_score: 900
              cur_oom_score: 100
              prev_capability_flags: 0
              cur_capability_flags: 4
              reason: OOM_ADJ_REASON_BIND_SERVICE
              seq_id: 10
            }
          }
        }
        # Pass seq_id = 11: provider acquired by worker on provider process
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2950
          track_event {
            type: TYPE_INSTANT
            name: "provider_acquired"
            [com.android.internal.FrameworksBaseTrackEvent.provider_state_changed_event] {
              authority: "com.example.provider.data"
              component_name: "com.example.provider.DataProvider"
              provider_id: 301
              pid: 300
              uid: 10003
              caller_pid: 200
              caller_uid: 10002
              is_stable: 1
              bind_id: 601
              seq_id: 11
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          track_event {
            type: TYPE_INSTANT
            name: "oom_adjuster_pass"
            [com.android.internal.FrameworksBaseTrackEvent.oom_adjuster_pass_event] {
              seq_id: 11
              reason: OOM_ADJ_REASON_GET_PROVIDER
              is_full_update: true
              top_pid: 100
              top_proc_state: PROCESS_STATE_TOP
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          track_event {
            type: TYPE_INSTANT
            name: "process_state_changed"
            [com.android.internal.FrameworksBaseTrackEvent.process_state_changed_event] {
              pid: 300
              uid: 10003
              prev_proc_state: PROCESS_STATE_CACHED_EMPTY
              cur_proc_state: PROCESS_STATE_BOUND_TOP
              prev_oom_score: 920
              cur_oom_score: 100
              prev_capability_flags: 0
              cur_capability_flags: 0
              reason: OOM_ADJ_REASON_GET_PROVIDER
              seq_id: 11
            }
          }
        }
        # End-of-trace snapshot
        packet {
          timestamp: 5000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_END
            record {
              pid: 100
              uid: 10001
              process_name: "com.example.top"
              start_seq_id: 1
              proc_state: 1002
              oom_score: 0
              capability_flags: 15
            }
            record {
              pid: 200
              uid: 10002
              process_name: "com.example.worker"
              start_seq_id: 2
              proc_state: 1020
              oom_score: 100
              capability_flags: 4
            }
            record {
              pid: 300
              uid: 10003
              process_name: "com.example.provider"
              start_seq_id: 3
              proc_state: 1020
              oom_score: 100
              capability_flags: 0
            }
            service {
              service_id: 201
              owning_pid: 200
              uid: 10002
              component_name: "com.example.worker/.SyncService"
              process_name: "com.example.worker"
            }
            service_binding {
              bind_id: 501
              service_id: 201
              intent_bind_id: 401
              client_pid: 100
              client_uid: 10001
              bind_flags_32: 1
              intent_action: "com.example.worker.ACTION_SYNC"
              component_name: "com.example.worker/.SyncService"
              host_pid: 200
              host_uid: 10002
            }
            provider {
              provider_id: 301
              owning_pid: 300
              uid: 10003
              authority: "com.example.provider.data"
              component_name: "com.example.provider.DataProvider"
              process_name: "com.example.provider"
            }
            provider_binding {
              bind_id: 601
              provider_id: 301
              client_pid: 200
              client_uid: 10002
              is_stable: true
              stable_count: 1
              unstable_count: 0
              authority: "com.example.provider.data"
              component_name: "com.example.provider.DataProvider"
              host_pid: 300
              host_uid: 10003
            }
          }
        }
        """),
        query="""
        SELECT
          s.id AS snap_id,
          s.ts,
          s.seq_id,
          s.reason,
          p.pid,
          p.proc_state,
          p.oom_score,
          (
            SELECT COUNT(*)
            FROM __intrinsic_android_process_state_service_binding b
            WHERE b.snapshot_id = s.id AND b.client_pid = 100
          ) AS svc_binds,
          (
            SELECT COUNT(*)
            FROM __intrinsic_android_process_state_provider_binding pb
            WHERE pb.snapshot_id = s.id AND pb.client_pid = 200
          ) AS prov_binds,
          (
            SELECT GROUP_CONCAT(te.action, '+')
            FROM __intrinsic_android_process_state_trigger_event te
            WHERE te.seq_id = s.seq_id AND te.kind != 'oom_adjuster_pass'
          ) AS triggers
        FROM __intrinsic_android_process_state_snapshot s
        JOIN __intrinsic_android_process_state_process p ON p.snapshot_id = s.id
        ORDER BY s.id, p.pid;
        """,
        out=Csv("""
          "snap_id","ts","seq_id","reason","pid","proc_state","oom_score","svc_binds","prov_binds","triggers"
          0,1900,0,"[NULL]",100,"TOP",0,0,0,"[NULL]"
          0,1900,0,"[NULL]",200,"CACHED_EMPTY",900,0,0,"[NULL]"
          0,1900,0,"[NULL]",300,"CACHED_EMPTY",920,0,0,"[NULL]"
          1,2000,10,"BIND_SERVICE",100,"TOP",0,1,0,"android.intent.action.SYNC_NOW+service_binding"
          1,2000,10,"BIND_SERVICE",200,"BOUND_TOP",100,1,0,"android.intent.action.SYNC_NOW+service_binding"
          1,2000,10,"BIND_SERVICE",300,"CACHED_EMPTY",920,1,0,"android.intent.action.SYNC_NOW+service_binding"
          2,3000,11,"GET_PROVIDER",100,"TOP",0,1,1,"provider_acquired"
          2,3000,11,"GET_PROVIDER",200,"BOUND_TOP",100,1,1,"provider_acquired"
          2,3000,11,"GET_PROVIDER",300,"BOUND_TOP",100,1,1,"provider_acquired"
        """))

  # A ServiceRecord / ContentProviderRecord outlives its host process: it can be
  # bound/acquired before the host exists (pid -1) and is re-created in a new
  # pid after the host dies. Owners must follow service_create /
  # provider_published, processes born from NONEXISTENT must not exist before
  # their birth, and service_binding_flags_updated must update the binding's
  # flags without ending it.
  def test_android_process_state_service_rehost(self):

    def instant(ts, name, ext, body):
      return f"""
        packet {{
          trusted_packet_sequence_id: 1
          timestamp: {ts}
          track_event {{
            type: TYPE_INSTANT
            name: "{name}"
            [com.android.internal.FrameworksBaseTrackEvent.{ext}] {{
              {body}
            }}
          }}
        }}"""

    def svc(ts, name, pid, seq, extra=''):
      return instant(
          ts, name, 'service_state_changed_event',
          f'component_name: "com.example.svc/.Svc" service_id: 7 pid: {pid} '
          f'uid: 10002 caller_pid: 100 caller_uid: 10001 seq_id: {seq} {extra}')

    def ps(ts, pid, prev, cur, seq):
      return instant(
          ts, 'process_state_changed', 'process_state_changed_event',
          f'pid: {pid} uid: 10002 prev_proc_state: PROCESS_STATE_{prev} '
          f'cur_proc_state: PROCESS_STATE_{cur} prev_oom_score: 0 '
          f'cur_oom_score: 200 reason: OOM_ADJ_REASON_BIND_SERVICE '
          f'seq_id: {seq}')

    def prov(ts, name, pid, seq, extra=''):
      return instant(
          ts, name, 'provider_state_changed_event',
          f'authority: "com.example.auth" '
          f'component_name: "com.example.svc/.Prov" provider_id: 9 '
          f'pid: {pid} uid: 10002 caller_pid: 100 caller_uid: 10001 '
          f'seq_id: {seq} {extra}')

    def oom(ts, seq, reason):
      return instant(
          ts, 'oom_adjuster_pass', 'oom_adjuster_pass_event',
          f'seq_id: {seq} reason: OOM_ADJ_REASON_{reason} '
          f'is_full_update: false')

    trace = ''.join([
        r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_START
            record { pid: 100 uid: 10001 process_name: "com.example.client" start_seq_id: 1 }
          }
        }
        packet {
          timestamp: 1000
          process_tree {
            processes { pid: 100 ppid: 1 cmdline: "com.example.client" uid: 10001 }
            processes { pid: 200 ppid: 1 cmdline: "com.example.svc" uid: 10002 }
            processes { pid: 250 ppid: 1 cmdline: "com.example.svc" uid: 10002 }
          }
        }""",
        # seq 1: bind + acquire before the host process exists.
        ps(1050, 100, 'CACHED_EMPTY', 'TOP', 1),
        svc(1100, 'service_binding', -1, 1, 'bind_id: 70 bind_flags_32: 1'),
        prov(1110, 'provider_acquired', -1, 1, 'bind_id: 90 is_stable: 1'),
        oom(1200, 1, 'BIND_SERVICE'),
        # seq 2: host 200 born, service created + provider published in it.
        ps(1300, 200, 'NONEXISTENT', 'CACHED_EMPTY', 2),
        svc(1310, 'service_create', 200, 2),
        prov(1320, 'provider_published', 200, 2),
        oom(1400, 2, 'PROCESS_BEGIN'),
        # seq 3: binding flags updated (AUTO_CREATE -> AUTO_CREATE|IMPORTANT).
        svc(1500, 'service_binding_flags_updated', 200, 3,
            'bind_id: 70 bind_flags_32: 65'),
        oom(1600, 3, 'BIND_SERVICE'),
        # seq 4: host dies; restart scheduled (no host).
        ps(1700, 200, 'CACHED_EMPTY', 'NONEXISTENT', 4),
        svc(1710, 'service_restart_scheduled', -1, 4),
        oom(1800, 4, 'PROCESS_END'),
        # seq 5: new host 250, service re-created there.
        ps(1900, 250, 'NONEXISTENT', 'CACHED_EMPTY', 5),
        svc(1910, 'service_create', 250, 5),
        oom(2000, 5, 'PROCESS_BEGIN'),
    ])
    return DiffTestBlueprint(
        trace=TextProto(trace),
        query="""
        SELECT
          s.seq_id,
          (
            SELECT GROUP_CONCAT(p.pid, ',')
            FROM __intrinsic_android_process_state_process p
            WHERE p.snapshot_id = s.id
          ) AS pids,
          (
            SELECT IFNULL(sv.owning_pid, 'none')
            FROM __intrinsic_android_process_state_service sv
            WHERE sv.snapshot_id = s.id
          ) AS svc_owner,
          (
            SELECT b.flags
            FROM __intrinsic_android_process_state_service_binding b
            WHERE b.snapshot_id = s.id
          ) AS bind_flags,
          (
            SELECT IFNULL(pv.owning_pid, 'none')
            FROM __intrinsic_android_process_state_provider pv
            WHERE pv.snapshot_id = s.id
          ) AS prov_owner,
          (
            SELECT COUNT(*)
            FROM __intrinsic_android_process_state_provider_binding pb
            WHERE pb.snapshot_id = s.id
          ) AS prov_binds
        FROM __intrinsic_android_process_state_snapshot s
        ORDER BY s.id;
        """,
        out=Csv("""
          "seq_id","pids","svc_owner","bind_flags","prov_owner","prov_binds"
          0,"100","[NULL]","[NULL]","[NULL]",0
          1,"100","none","AUTO_CREATE","none",1
          2,"100,200",200,"AUTO_CREATE",200,1
          3,"100,200",200,"AUTO_CREATE | IMPORTANT",200,1
          4,"100","none","AUTO_CREATE | IMPORTANT","[NULL]",0
          5,"100,250",250,"AUTO_CREATE | IMPORTANT","[NULL]",0
        """))

  def test_android_process_state_trigger_links(self):

    def instant(ts, name, ext, body):
      return f"""
        packet {{
          trusted_packet_sequence_id: 1
          timestamp: {ts}
          track_event {{
            type: TYPE_INSTANT
            name: "{name}"
            [com.android.internal.FrameworksBaseTrackEvent.{ext}] {{
              {body}
            }}
          }}
        }}"""

    def svc(ts, name, seq, extra=''):
      return instant(
          ts, name, 'service_state_changed_event',
          f'component_name: "com.example.svc/.Svc" service_id: 7 pid: 200 '
          f'uid: 10002 caller_pid: 100 caller_uid: 10001 seq_id: {seq} '
          f'{extra}')

    def fgs(ts, name, seq, fg_type):
      return instant(
          ts, name, 'fg_service_state_changed_event',
          f'component_name: "com.example.svc/.Svc" service_id: 7 pid: 200 '
          f'uid: 10002 foreground_service_type: {fg_type} seq_id: {seq}')

    def prov(ts, name, seq, extra=''):
      return instant(
          ts, name, 'provider_state_changed_event',
          f'authority: "com.example.auth" '
          f'component_name: "com.example.svc/.Prov" provider_id: 9 '
          f'pid: 200 uid: 10002 caller_pid: 100 caller_uid: 10001 '
          f'seq_id: {seq} {extra}')

    def oom(ts, seq):
      return instant(
          ts, 'oom_adjuster_pass', 'oom_adjuster_pass_event',
          f'seq_id: {seq} reason: OOM_ADJ_REASON_BIND_SERVICE '
          f'is_full_update: false')

    trace = ''.join([
        r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_START
            record { pid: 100 uid: 10001 process_name: "com.example.client" start_seq_id: 1 }
            record { pid: 200 uid: 10002 process_name: "com.example.svc" start_seq_id: 1 }
          }
        }
        packet {
          timestamp: 1000
          process_tree {
            processes { pid: 100 ppid: 1 cmdline: "com.example.client" uid: 10001 }
            processes { pid: 200 ppid: 1 cmdline: "com.example.svc" uid: 10002 }
          }
        }""",
        # seq 1: create + bind + publish provider + stable acquire.
        svc(1100, 'service_create', 1),
        svc(1110, 'service_binding', 1,
            'bind_id: 70 intent_bind_id: 71 bind_flags_32: 1'),
        prov(1120, 'provider_published', 1),
        prov(1130, 'provider_acquired', 1, 'bind_id: 90 is_stable: 1'),
        oom(1200, 1),
        # seq 2: provider connection drops to unstable-only; FGS starts.
        prov(1300, 'provider_connection_updated', 2,
             'bind_id: 90 is_stable: 0'),
        fgs(1310, 'fgs_start', 2, 8),
        oom(1400, 2),
        # seq 3: FGS type changes (8 -> 0x48), back to stable.
        fgs(1500, 'fgs_type_changed', 3, 72),
        prov(1510, 'provider_connection_updated', 3,
             'bind_id: 90 is_stable: 1'),
        oom(1600, 3),
        # seq 4: FGS stops, provider released, unbind.
        fgs(1700, 'fgs_stop', 4, 0),
        prov(1710, 'provider_released', 4, 'bind_id: 90 is_stable: 1'),
        svc(1720, 'service_unbinding', 4, 'bind_id: 70 intent_bind_id: 71'),
        oom(1800, 4),
        r"""
        packet {
          timestamp: 2000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_END
            record { pid: 100 uid: 10001 process_name: "com.example.client" start_seq_id: 1 proc_state: 2 oom_score: 0 }
            record { pid: 200 uid: 10002 process_name: "com.example.svc" start_seq_id: 1 proc_state: 10 oom_score: 200 }
            service { service_id: 7 owning_pid: 200 uid: 10002 component_name: "com.example.svc/.Svc" }
            provider { provider_id: 9 owning_pid: 200 uid: 10002 authority: "com.example.auth" component_name: "com.example.svc/.Prov" }
          }
        }""",
    ])
    return DiffTestBlueprint(
        trace=TextProto(trace),
        query="""
        SELECT
          t.seq_id,
          t.kind,
          t.detail,
          t.service_id,
          t.bind_id,
          t.intent_bind_id,
          t.provider_id,
          sv.is_foreground AS fg,
          sv.foreground_service_type AS fg_type,
          (
            SELECT pb.stable
            FROM __intrinsic_android_process_state_provider_binding pb
            WHERE pb.snapshot_id = s.id AND pb.bind_id = 90
          ) AS prov_stable,
          (
            SELECT COUNT(*)
            FROM __intrinsic_android_process_state_service_binding b
            WHERE b.snapshot_id = s.id AND b.bind_id = 70
          ) AS svc_bound
        FROM __intrinsic_android_process_state_trigger_event t
        JOIN __intrinsic_android_process_state_snapshot s USING (seq_id)
        LEFT JOIN __intrinsic_android_process_state_service sv
          ON sv.snapshot_id = s.id AND sv.svc_id = 7
        WHERE t.kind != 'oom_adjuster_pass'
        ORDER BY t.ts;
        """,
        out=Csv("""
        "seq_id","kind","detail","service_id","bind_id","intent_bind_id","provider_id","fg","fg_type","prov_stable","svc_bound"
        1,"service_create","[NULL]",7,"[NULL]","[NULL]","[NULL]",0,0,1,1
        1,"service_binding","AUTO_CREATE",7,70,71,"[NULL]",0,0,1,1
        1,"provider_published","com.example.svc/.Prov","[NULL]","[NULL]","[NULL]",9,0,0,1,1
        1,"provider_acquired","com.example.svc/.Prov (stable)","[NULL]",90,"[NULL]",9,0,0,1,1
        2,"provider_connection_updated","com.example.svc/.Prov (unstable)","[NULL]",90,"[NULL]",9,1,8,0,1
        2,"fgs_start","fgs_type=0x8",7,"[NULL]","[NULL]","[NULL]",1,8,0,1
        3,"fgs_type_changed","fgs_type=0x48",7,"[NULL]","[NULL]","[NULL]",1,72,1,1
        3,"provider_connection_updated","com.example.svc/.Prov (stable)","[NULL]",90,"[NULL]",9,1,72,1,1
        4,"fgs_stop","fgs_type=0x0",7,"[NULL]","[NULL]","[NULL]",0,0,"[NULL]",0
        4,"provider_released","com.example.svc/.Prov (stable)","[NULL]",90,"[NULL]",9,0,0,"[NULL]",0
        4,"service_unbinding","[NULL]",7,70,71,"[NULL]",0,0,"[NULL]",0
        """))

  def test_android_process_state_unhosted_service(self):

    def instant(ts, name, ext, body):
      return f"""
        packet {{
          trusted_packet_sequence_id: 1
          timestamp: {ts}
          track_event {{
            type: TYPE_INSTANT
            name: "{name}"
            [com.android.internal.FrameworksBaseTrackEvent.{ext}] {{
              {body}
            }}
          }}
        }}"""

    def svc(ts, name, seq, pid, extra=''):
      return instant(
          ts, name, 'service_state_changed_event',
          f'component_name: "com.example.svc/.Svc" service_id: 8 pid: {pid} '
          f'uid: 10002 caller_pid: 100 caller_uid: 10001 seq_id: {seq} '
          f'{extra}')

    def oom(ts, seq):
      return instant(
          ts, 'oom_adjuster_pass', 'oom_adjuster_pass_event',
          f'seq_id: {seq} reason: OOM_ADJ_REASON_BIND_SERVICE '
          f'is_full_update: false')

    trace = ''.join([
        r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_START
            record { pid: 100 uid: 10001 process_name: "com.example.client" start_seq_id: 1 }
            record { pid: 200 uid: 10002 process_name: "com.example.svc" start_seq_id: 1 }
          }
        }
        packet {
          timestamp: 1000
          process_tree {
            processes { pid: 100 ppid: 1 cmdline: "com.example.client" uid: 10001 }
            processes { pid: 200 ppid: 1 cmdline: "com.example.svc" uid: 10002 }
          }
        }""",
        # seq 1: bound without AUTO_CREATE while the service is not running.
        svc(1100, 'service_binding', 1, -1,
            'bind_id: 80 intent_bind_id: 81 bind_flags_32: 64'),
        oom(1200, 1),
        # seq 2: started -> created in pid 200.
        svc(1300, 'service_start', 2, -1),
        svc(1310, 'service_create', 2, 200),
        oom(1400, 2),
        # seq 3: stopped + destroyed; the connection is still bound.
        svc(1500, 'service_stop', 3, 200),
        svc(1510, 'service_destroy', 3, 200),
        oom(1600, 3),
        # seq 4: unbound -> the record goes away.
        svc(1700, 'service_unbinding', 4, -1, 'bind_id: 80 intent_bind_id: 81'),
        oom(1800, 4),
        # seq 5: host process dies.
        instant(
            1900, 'process_state_died', 'process_state_died_event',
            'pid: 200 uid: 10002 prev_proc_state: PROCESS_STATE_CACHED_EMPTY '
            'seq_id: 5'),
        oom(1950, 5),
        r"""
        packet {
          timestamp: 2000
          [com.android.internal.FrameworksBaseTracePacket.android_process_state] {
            dump_reason: DUMP_REASON_END
            record { pid: 100 uid: 10001 process_name: "com.example.client" start_seq_id: 1 proc_state: 2 oom_score: 0 }
          }
        }""",
    ])
    return DiffTestBlueprint(
        trace=TextProto(trace),
        query="""
        SELECT
          s.seq_id,
          (
            SELECT GROUP_CONCAT(t.kind, ',')
            FROM __intrinsic_android_process_state_trigger_event t
            WHERE t.seq_id = s.seq_id AND t.kind != 'oom_adjuster_pass'
          ) AS triggers,
          sv.svc_id IS NOT NULL AS svc_present,
          sv.owning_pid,
          sv.start_requested,
          (
            SELECT COUNT(*)
            FROM __intrinsic_android_process_state_service_binding b
            WHERE b.snapshot_id = s.id AND b.bind_id = 80
          ) AS bound,
          (
            SELECT COUNT(*)
            FROM __intrinsic_android_process_state_process p
            WHERE p.snapshot_id = s.id AND p.pid = 200
          ) AS host_alive
        FROM __intrinsic_android_process_state_snapshot s
        LEFT JOIN __intrinsic_android_process_state_service sv
          ON sv.snapshot_id = s.id AND sv.svc_id = 8
        WHERE s.seq_id > 0
        ORDER BY s.seq_id;
        """,
        out=Csv("""
        "seq_id","triggers","svc_present","owning_pid","start_requested","bound","host_alive"
        1,"service_binding",1,"[NULL]",0,1,1
        2,"service_start,service_create",1,200,1,1,1
        3,"service_stop,service_destroy",1,"[NULL]",0,1,1
        4,"service_unbinding",0,"[NULL]","[NULL]",0,1
        5,"process_state_died",0,"[NULL]","[NULL]",0,0
        """))
