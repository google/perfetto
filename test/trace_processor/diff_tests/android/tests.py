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


class Android(TestSuite):

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
        SELECT t.type, t.name, c.id, c.ts, c.type, c.value
        FROM counter_track t JOIN counter c ON t.id = c.track_id
        WHERE name = 'ScreenState';
        """,
        out=Csv("""
        "type","name","id","ts","type","value"
        "counter_track","ScreenState",0,1000,"counter",2.000000
        "counter_track","ScreenState",1,2000,"counter",1.000000
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
        SELECT t.type, t.name, s.id, s.ts, s.dur, s.type, s.name
        FROM track t JOIN slice s ON s.track_id = t.id
        WHERE t.name = 'DeviceStateChanged';
        """,
        out=Path('android_system_property_slice.out'))

  def test_android_battery_stats_event_slices(self):
    # The following has three events
    # * top (123, mail) from 1000 to 9000 explicit
    # * job (456, mail_job) starting at 3000 (end is inferred as trace end)
    # * job (789, video_job) ending at 4000 (start is inferred as trace start)
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "N|1000|battery_stats.top|+top=123:\"mail\"\n"
              }
            }
            event {
              timestamp: 3000
              pid: 1
              print {
                buf: "N|1000|battery_stats.job|+job=456:\"mail_job\"\n"
              }
            }
            event {
              timestamp: 4000
              pid: 1
              print {
                buf: "N|1000|battery_stats.job|-job=789:\"video_job\"\n"
              }
            }
            event {
              timestamp: 9000
              pid: 1
              print {
                buf: "N|1000|battery_stats.top|-top=123:\"mail\"\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT IMPORT('android.battery_stats');
        SELECT * FROM android_battery_stats_event_slices
        ORDER BY str_value;
        """,
        out=Path('android_battery_stats_event_slices.out'))

  def test_android_battery_stats_counters(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|13\n"
              }
            }
            event {
              timestamp: 4000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|20\n"
              }
            }
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.audio|1\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT IMPORT('android.battery_stats');
        SELECT * FROM android_battery_stats_state
        ORDER BY ts, track_name;
        """,
        out=Path('android_battery_stats_state.out'))

  def test_android_network_activity(self):
    # The following should have three activity regions:
    # * uid=123 from 1000 to 2010 (note: end is max(ts)+idle_ns)
    # * uid=456 from 1005 to 2015 (note: doesn't group with above due to name)
    # * uid=123 from 3000 to 5500 (note: gap between 1010 to 3000 > idle_ns)
    # Note: packet_timestamps are delta encoded from the base timestamp.
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          network_packet_bundle {
            ctx {
              direction: DIR_EGRESS
              interface: "wlan"
              uid: 123
            }
            packet_timestamps: [
              1000, 1010,
              3000, 3050, 4000, 4500
            ],
            packet_lengths: [
              50, 50,
              50, 50, 50, 50
            ],
          }
        }
        packet {
          timestamp: 0
          network_packet_bundle {
            ctx {
              direction: DIR_EGRESS
              interface: "wlan"
              uid: 456
            }
            packet_timestamps: [1005, 1015]
            packet_lengths: [100, 200]
          }
        }
        packet {
          timestamp: 0
          network_packet_bundle {
            ctx {
              direction: DIR_INGRESS
              interface: "loopback"
              uid: 123
            }
            packet_timestamps: [6000]
            packet_lengths: [100]
          }
        }
        """),
        query="""
        SELECT RUN_METRIC(
          'android/network_activity_template.sql',
          'view_name', 'android_network_activity',
          'group_by',  'package_name',
          'filter',    'iface = "wlan"',
          'idle_ns',   '1000',
          'quant_ns',  '100'
        );

        SELECT * FROM android_network_activity
        ORDER BY package_name, ts;
        """,
        out=Path('android_network_activity.out'))

  def test_binder_sync_binder_metrics(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        SELECT IMPORT('android.binder');
        SELECT
          aidl_name,
          binder_txn_id,
          client_process,
          client_thread,
          client_upid,
          client_utid,
          client_tid,
          is_main_thread,
          client_ts,
          client_dur,
          binder_reply_id,
          server_process,
          server_thread,
          server_upid,
          server_utid,
          server_tid,
          server_ts,
          server_dur
        FROM android_sync_binder_metrics_by_txn
        WHERE binder_txn_id = 34382
        ORDER BY client_ts
        LIMIT 1;
      """,
        out=Csv("""
        "aidl_name","binder_txn_id","client_process","client_thread","client_upid","client_utid","client_tid","is_main_thread","client_ts","client_dur","binder_reply_id","server_process","server_thread","server_upid","server_utid","server_tid","server_ts","server_dur"
        "AIDL::java::ISensorPrivacyManager::isSensorPrivacyEnabled::server",34382,"/system/bin/audioserver","audioserver",281,281,492,1,25505818197,3125407,34383,"system_server","binder:641_4",311,539,1596,25505891588,3000749
      """))

  def test_binder_sync_binder_thread_state(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
      SELECT IMPORT('android.binder');
      SELECT
        binder_txn_id,
        client_ts,
        client_tid,
        binder_reply_id,
        server_ts,
        server_tid,
        thread_state_type,
        thread_state,
        thread_state_dur,
        thread_state_count
      FROM android_sync_binder_thread_state_by_txn
      WHERE binder_txn_id = 34382
      ORDER BY thread_state_dur;
      """,
        out=Csv("""
      "binder_txn_id","client_ts","client_tid","binder_reply_id","server_ts","server_tid","thread_state_type","thread_state","thread_state_dur","thread_state_count"
      34382,25505818197,492,34383,25505891588,1596,"binder_reply","R+",10030,1
      34382,25505818197,492,34383,25505891588,1596,"binder_txn","Running",26597,2
      34382,25505818197,492,34383,25505891588,1596,"binder_txn","R",38947,1
      34382,25505818197,492,34383,25505891588,1596,"binder_reply","Running",533663,3
      34382,25505818197,492,34383,25505891588,1596,"binder_reply","D",864664,1
      34382,25505818197,492,34383,25505891588,1596,"binder_reply","R",1592392,1
      34382,25505818197,492,34383,25505891588,1596,"binder_txn","S",3059863,1
      """))

  def test_binder_sync_binder_blocked_function(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
      SELECT IMPORT('android.binder');
      SELECT
        binder_txn_id,
        client_ts,
        client_tid,
        binder_reply_id,
        server_ts,
        server_tid,
        thread_state_type,
        blocked_function,
        blocked_function_dur,
        blocked_function_count
      FROM android_sync_binder_blocked_functions_by_txn
      WHERE binder_txn_id = 34382
      ORDER BY blocked_function_dur;
      """,
        out=Csv("""
      "binder_txn_id","client_ts","client_tid","binder_reply_id","server_ts","server_tid","thread_state_type","blocked_function","blocked_function_dur","blocked_function_count"
      34382,25505818197,492,34383,25505891588,1596,"binder_reply","filemap_fault",864664,1
      """))

  def test_binder_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query=Metric('android_binder'),
        out=Path('android_binder_metric.out'))

  def test_android_blocking_calls_cuj(self):
    return DiffTestBlueprint(
        trace=Path('android_blocking_calls_cuj_metric.py'),
        query=Metric('android_blocking_calls_cuj_metric'),
        out=Path('android_blocking_calls_cuj_metric.out'))

  def test_android_blocking_calls_on_jank_cujs(self):
    return DiffTestBlueprint(
        trace=Path('../graphics/android_jank_cuj.py'),
        query=Metric('android_blocking_calls_cuj_metric'),
        out=Path('android_blocking_calls_on_jank_cuj_metric.out'))

  def test_android_slices_standardization_for_aggregation(self):
    return DiffTestBlueprint(
        trace=Path('android_slice_standardization.py'),
        query="""
        SELECT IMPORT('android.slices');
        SELECT ANDROID_STANDARDIZE_SLICE_NAME(slice.name) name
        FROM slice
        ORDER BY name;
        """,
        out=Path('android_slice_standardization.out'))

  def test_monitor_contention_extraction(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      SELECT IMPORT('android.monitor_contention');
      SELECT
        blocking_method,
        blocked_method,
        short_blocking_method,
        short_blocked_method
      FROM android_monitor_contention
      WHERE binder_reply_id IS NOT NULL
      ORDER BY dur DESC
      LIMIT 1;
      """,
        out=Csv("""
        "blocking_method","blocked_method","short_blocking_method","short_blocked_method"
        "boolean com.android.server.am.ActivityManagerService.forceStopPackageLocked(java.lang.String, int, boolean, boolean, boolean, boolean, boolean, int, java.lang.String)","boolean com.android.server.am.ActivityManagerService.isUidActive(int, java.lang.String)","com.android.server.am.ActivityManagerService.forceStopPackageLocked","com.android.server.am.ActivityManagerService.isUidActive"
      """))

  def test_monitor_contention_chain_blocked_functions(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      SELECT IMPORT('android.monitor_contention');
      SELECT
        *
      FROM android_monitor_contention_chain_blocked_functions_by_txn
      WHERE id = 13934
      ORDER BY blocked_function_dur;
      """,
        out=Csv("""
        "id","blocked_function","blocked_function_dur","blocked_function_count"
        13934,"blkdev_issue_flush",11950576,1
      """))

  def test_monitor_contention_chain_thread_states(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      SELECT IMPORT('android.monitor_contention');
      SELECT
        *
      FROM android_monitor_contention_chain_thread_state_by_txn
      WHERE id = 13934
      ORDER BY thread_state_dur;
      """,
        out=Csv("""
        "id","thread_state","thread_state_dur","thread_state_count"
        13934,"R+",7649,1
        13934,"R",300606,3
        13934,"Running",649961,3
        13934,"D",11950576,1
      """))

  def test_monitor_contention_chain_extraction_parent(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      SELECT IMPORT('android.monitor_contention');
      SELECT * FROM android_monitor_contention_chain
        WHERE parent_id IS NOT NULL
      ORDER BY dur DESC
      LIMIT 1;
      """,
        out=Csv("""
        "parent_id","blocking_method","blocked_method","short_blocking_method","short_blocked_method","blocking_src","blocked_src","waiter_count","blocked_utid","blocked_thread_name","blocking_utid","blocking_thread_name","blocking_tid","upid","process_name","id","ts","dur","track_id","is_blocked_thread_main","blocked_thread_tid","is_blocking_thread_main","blocking_thread_tid","binder_reply_id","binder_reply_ts","binder_reply_tid","pid"
        956,"void com.android.server.am.AppProfiler.collectPssInBackground()","void com.android.server.am.ProcessRecord.setPid(int)","com.android.server.am.AppProfiler.collectPssInBackground","com.android.server.am.ProcessRecord.setPid","AppProfiler.java:514","ProcessRecord.java:596",0,656,"binder:642_12",506,"android.bg",670,250,"system_server",949,1737122781871,7301144,1236,0,2720,0,670,"[NULL]","[NULL]","[NULL]",642
      """))

  def test_monitor_contention_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query=Metric('android_monitor_contention'),
        out=Path('android_monitor_contention.out'))
