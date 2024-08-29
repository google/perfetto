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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class AndroidStdlib(TestSuite):

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
        INCLUDE PERFETTO MODULE android.battery_stats;
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
        INCLUDE PERFETTO MODULE android.battery_stats;
        SELECT * FROM android_battery_stats_state
        ORDER BY ts, track_name;
        """,
        out=Csv("""
        "ts","dur","safe_dur","track_name","value","value_name"
        1000,-1,3000,"battery_stats.audio",1,"active"
        1000,3000,3000,"battery_stats.data_conn",13,"4G (LTE)"
        4000,-1,0,"battery_stats.data_conn",20,"5G (NR)"
        """))

  def test_anrs(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/android/android_anr_metric.py'),
        query="""
        INCLUDE PERFETTO MODULE android.anrs;
        SELECT *
        FROM android_anrs;
      """,
        out=Csv("""
        "process_name","pid","upid","error_id","ts","subject"
        "com.google.android.app1",11167,"[NULL]","da24554c-452a-4ae1-b74a-fb898f6e0982",1000,"Test ANR subject 1"
        "com.google.android.app2","[NULL]","[NULL]","8612fece-c2f1-4aeb-9d45-8e6d9d0201cf",2000,"Test ANR subject 2"
        "com.google.android.app3","[NULL]","[NULL]","c25916a0-a8f0-41f3-87df-319e06471a0f",3000,"[NULL]"
      """))

  def test_binder_sync_binder_metrics(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
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
          client_oom_score,
          binder_reply_id,
          server_process,
          server_thread,
          server_upid,
          server_utid,
          server_tid,
          server_ts,
          server_dur,
          server_oom_score
        FROM android_binder_txns
        WHERE binder_txn_id = 34382
        ORDER BY client_ts
        LIMIT 1;
      """,
        out=Csv("""
        "aidl_name","binder_txn_id","client_process","client_thread","client_upid","client_utid","client_tid","is_main_thread","client_ts","client_dur","client_oom_score","binder_reply_id","server_process","server_thread","server_upid","server_utid","server_tid","server_ts","server_dur","server_oom_score"
        "AIDL::java::ISensorPrivacyManager::isSensorPrivacyEnabled::server",34382,"/system/bin/audioserver","audioserver",281,281,492,1,25505818197,3125407,-1000,34383,"system_server","binder:641_4",311,539,1596,25505891588,3000749,-900
      """))

  def test_binder_sync_binder_thread_state(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE android.binder;
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
      INCLUDE PERFETTO MODULE android.binder;
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

  def test_android_slices_standardization_for_aggregation(self):
    return DiffTestBlueprint(
        trace=Path('android_slice_standardization.py'),
        query="""
        INCLUDE PERFETTO MODULE android.slices;
        SELECT ANDROID_STANDARDIZE_SLICE_NAME(slice.name) name
        FROM slice
        """,
        out=Csv("""
        "name"
        "Lock contention on thread list lock <...>"
        "Lock contention on thread suspend count lock <...>"
        "Lock contention on a monitor lock <...>"
        "monitor contention with <...>"
        "SuspendThreadByThreadId <...>"
        "LoadApkAssetsFd <...>"
        "relayoutWindow <...>"
        "CoroutineContinuation"
        "Choreographer#doFrame"
        "DrawFrames"
        "APK load"
        "OpenDexFilesFromOat"
        "Open oat file"
        "CoroutineContinuation"
        "Garbage Collector"
        "Handler: android.view.View"
        "Handler: android.os.AsyncTask"
        "Handler: com.android.systemui.broadcast.ActionReceiver"
        "Handler: com.android.keyguard.KeyguardUpdateMonitor"
        "Handler: com.android.systemui.qs.TileServiceManager"
        """))

  def test_monitor_contention_extraction(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE android.monitor_contention;
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
      INCLUDE PERFETTO MODULE android.monitor_contention;
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
      INCLUDE PERFETTO MODULE android.monitor_contention;
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

  def test_monitor_contention_chain_extraction(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE android.monitor_contention;
      SELECT * FROM android_monitor_contention_chain
        WHERE parent_id IS NOT NULL
      ORDER BY dur DESC
      LIMIT 1;
      """,
        out=Csv("""
        "parent_id","blocking_method","blocked_method","short_blocking_method","short_blocked_method","blocking_src","blocked_src","waiter_count","blocked_utid","blocked_thread_name","blocking_utid","blocking_thread_name","blocking_tid","upid","process_name","id","ts","dur","monotonic_dur","track_id","is_blocked_thread_main","blocked_thread_tid","is_blocking_thread_main","blocking_thread_tid","binder_reply_id","binder_reply_ts","binder_reply_tid","pid","child_id"
        949,"void com.android.server.am.ActivityManagerService$AppDeathRecipient.binderDied()","int com.android.server.am.ActivityManagerService.getMemoryTrimLevel()","com.android.server.am.ActivityManagerService$AppDeathRecipient.binderDied","com.android.server.am.ActivityManagerService.getMemoryTrimLevel","ActivityManagerService.java:1478","ActivityManagerService.java:9183",1,250,"system_server",656,"binder:642_12",2720,250,"system_server",956,1737123891932,17577143,17577143,1215,1,642,0,2720,"[NULL]","[NULL]","[NULL]",642,"[NULL]"
      """))

  def test_monitor_contention_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.monitor_contention;

        SELECT HEX(pprof) FROM android_monitor_contention_graph(303)
      """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
        Sample:
        Values: 29604
        Stack:
        android.bg:android.os.MessageQueue.nativeWake (0x0)
        fg:android.os.MessageQueue.next (0x0)

        Sample:
        Values: 66924
        Stack:
        android.bg:android.os.MessageQueue.enqueueMessage (0x0)
        fg:android.os.MessageQueue.next (0x0)

        Sample:
        Values: 73265
        Stack:
        main:android.os.MessageQueue.enqueueMessage (0x0)
        fg:android.os.MessageQueue.next (0x0)
        """))

  def test_thread_creation_spam(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE android.thread;
      SELECT * FROM _android_thread_creation_spam(1e9, 1e9);
      """,
        out=Csv("""
      "process_name","pid","thread_name_prefix","max_count_per_sec"
      "com.android.providers.media.module",3487,"SharedPreferenc",3
      "com.android.providers.media.module",3487,"MediaCodec_loop",2
      "/apex/com.android.adbd/bin/adbd",527,"shell",1
      "media.swcodec",563,"id.hevc.decoder",1
      "system_server",642,"Thread",1
      "sh",3474,"sh",1
      "sh",3476,"sh",1
      "sh",3478,"sh",1
      "am",3480,"am",1
      "cmd",3482,"binder",1
      "cmd",3482,"cmd",1
      "com.android.providers.media.module",3487,"CodecLooper",1
      "sh",3517,"sh",1
      "sgdisk",3521,"sgdisk",1
      "blkid",3523,"blkid",1
      "binder:243_4",3524,"binder",1
      "fsck_msdos",3525,"fsck_msdos",1
      "binder:243_4",3526,"binder",1
      "sh",3532,"sh",1
      "cut",3534,"cut",1
      "sh",3536,"sh",1
      "sh",3544,"sh",1
      "sh",3546,"sh",1
      "sh",3564,"sh",1
      """))

  def test_f2fs_counter_stats(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE android.io;
      SELECT * FROM _android_io_f2fs_counter_stats;
      """,
        out=Csv("""
        "name","sum","max","min","dur","count","avg"
        "read_app_total",580966.000000,567184.000000,13782.000000,2515275969,2,290483.000000
        "read_app_buffered",580966.000000,567184.000000,13782.000000,2515275969,2,290483.000000
        "write_cp_node",94208.000000,94208.000000,0.000000,2515275969,2,47104.000000
        "write_app_mapped",65536.000000,65536.000000,0.000000,2515275969,2,32768.000000
        "write_fs_data",28672.000000,28672.000000,0.000000,2515275969,2,14336.000000
        "write_cp_meta",28672.000000,28672.000000,0.000000,2515275969,2,14336.000000
        "write_app_total",20616.000000,20616.000000,0.000000,2515275969,2,10308.000000
        "write_app_buffered",20616.000000,20616.000000,0.000000,2515275969,2,10308.000000
        "write_fs_node",8192.000000,8192.000000,0.000000,2515275969,2,4096.000000
        "write_sync_meta_peak",8.000000,8.000000,0.000000,2515276848,2,4.000000
        "write_sync_meta_cnt",5.000000,5.000000,0.000000,2515276848,2,2.500000
        "write_sync_node_peak",4.000000,4.000000,0.000000,2515276848,2,2.000000
        "write_sync_node_cnt",3.000000,3.000000,0.000000,2515276848,2,1.500000
        "write_sync_data_cnt",3.000000,3.000000,0.000000,2515276848,2,1.500000
        "write_sync_node_avg",1.000000,1.000000,0.000000,2515276848,2,0.500000
        "write_sync_meta_avg",1.000000,1.000000,0.000000,2515276848,2,0.500000
        "write_sync_data_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_sync_data_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_node_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_node_cnt",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_node_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_meta_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_meta_cnt",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_meta_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_data_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_data_cnt",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_async_data_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_node_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_node_cnt",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_node_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_meta_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_meta_cnt",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_meta_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_data_peak",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_data_cnt",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "read_data_avg",0.000000,0.000000,0.000000,2515276848,2,0.000000
        "write_gc_node",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "write_gc_data",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "write_fs_meta",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "write_cp_data",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "write_app_direct",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_fs_node",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_fs_meta",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_fs_gdata",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_fs_data",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_fs_cdata",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_app_mapped",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "read_app_direct",0.000000,0.000000,0.000000,2515275969,2,0.000000
        "other_fs_discard",0.000000,0.000000,0.000000,2515275969,2,0.000000
      """))

  def test_f2fs_write_stats(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE android.io;
      SELECT tid, thread_name, pid, process_name, ino, dev, bytes, write_count FROM _android_io_f2fs_write_stats;
      """,
        out=Csv("""
        "tid","thread_name","pid","process_name","ino","dev","bytes","write_count"
        3548,"AsyncTask #1",3487,"com.android.providers.media.module",2636,65077,135168,33
        3516,"fg",3487,"com.android.providers.media.module",2409,65077,98304,24
        3548,"AsyncTask #1",3487,"com.android.providers.media.module",2642,65077,78280,57
        3516,"fg",3487,"com.android.providers.media.module",2424,65077,37112,28
        3487,"rs.media.module",3487,"com.android.providers.media.module",2366,65077,16480,12
        3515,"ackgroundThread",3487,"com.android.providers.media.module",2642,65077,8272,7
        282,"f2fs_ckpt-254:5",282,"f2fs_ckpt-254:5",4,65077,432,6
        282,"f2fs_ckpt-254:5",282,"f2fs_ckpt-254:5",5,65077,432,6
        3548,"AsyncTask #1",3487,"com.android.providers.media.module",3145,65077,233,2
        743,"StorageManagerS",642,"system_server",3144,65077,227,1
        282,"f2fs_ckpt-254:5",282,"f2fs_ckpt-254:5",6,65077,216,3
        3487,"rs.media.module",3487,"com.android.providers.media.module",2367,65077,8,8
        3516,"fg",3487,"com.android.providers.media.module",2425,65077,8,8
        3548,"AsyncTask #1",3487,"com.android.providers.media.module",2643,65077,8,8
      """))

  def test_f2fs_aggregate_write_stats(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.io;
        SELECT total_write_count, distinct_processes, total_bytes_written,
               distinct_device_count, distinct_inode_count, distinct_thread_count
        FROM _android_io_f2fs_aggregate_write_stats
        """,
        out=Csv("""
        "total_write_count","distinct_processes","total_bytes_written","distinct_device_count","distinct_inode_count","distinct_thread_count"
        203,3,375180,1,13,6
        """))

  def test_binder_async_txns(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
        SELECT
          aidl_name,
          client_process,
          server_process,
          client_thread,
          client_tid,
          server_tid,
          is_main_thread,
          client_oom_score,
          server_oom_score,
          client_ts,
          server_ts,
          aidl_ts,
          aidl_dur
        FROM android_binder_txns
        WHERE aidl_name IS NOT NULL AND is_sync = 0
        ORDER BY client_ts
        LIMIT 10;
      """,
        out=Csv("""
        "aidl_name","client_process","server_process","client_thread","client_tid","server_tid","is_main_thread","client_oom_score","server_oom_score","client_ts","server_ts","aidl_ts","aidl_dur"
        "AIDL::cpp::IClientCallback::onClients::cppServer","/system/bin/servicemanager","/system/bin/apexd","servicemanager",243,386,1,-1000,-1000,22213481492,22213517474,22213598784,322601
        "AIDL::cpp::IMediaMetricsService::submitBuffer::cppServer","/system/bin/audioserver","media.metrics","audioserver",492,1262,1,-1000,-1000,25512325446,25512488255,25512708792,9677878
        "AIDL::cpp::IMediaMetricsService::submitBuffer::cppServer","/system/bin/audioserver","media.metrics","audioserver",492,1262,1,-1000,-1000,25512842465,25522410505,25522418582,58044
        "AIDL::cpp::IDisplayEventConnection::stealReceiveChannel::cppServer","/vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu","/system/bin/surfaceflinger","binder:446_1",553,522,0,-1000,-1000,25847718645,25847734867,25849056936,10493
        "AIDL::cpp::ITunnelModeEnabledListener::onTunnelModeEnabledChanged::cppServer","/system/bin/surfaceflinger","system_server","binder:496_2",522,1600,0,-1000,-900,25854181504,25854195485,25854205007,214767
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,522,0,-900,-1000,25855697394,25855710732,25855721528,81461
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,522,0,-900,-1000,25873210999,25873224961,25873232951,3493
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,522,0,-900,-1000,25951278287,25952242397,25952255710,30672
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,1575,0,-900,-1000,25965452828,25965590137,25965596757,2631
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,1575,0,-900,-1000,26046376252,26046544680,26046553099,53452
      """))

  def test_binder_txns(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
        SELECT
          aidl_name,
          client_process,
          server_process,
          client_thread,
          client_tid,
          server_tid,
          is_main_thread,
          client_oom_score,
          server_oom_score,
          client_ts,
          server_ts,
          client_dur,
          server_dur,
          client_monotonic_dur,
          server_monotonic_dur,
          aidl_ts,
          aidl_dur,
          is_sync,
          client_package_version_code,
          server_package_version_code,
          is_client_package_debuggable,
          is_server_package_debuggable
        FROM android_binder_txns
        WHERE aidl_name IS NOT NULL AND client_package_version_code IS NOT NULL
        ORDER BY client_ts
        LIMIT 10;
      """,
        out=Csv("""
        "aidl_name","client_process","server_process","client_thread","client_tid","server_tid","is_main_thread","client_oom_score","server_oom_score","client_ts","server_ts","client_dur","server_dur","client_monotonic_dur","server_monotonic_dur","aidl_ts","aidl_dur","is_sync","client_package_version_code","server_package_version_code","is_client_package_debuggable","is_server_package_debuggable"
        "AIDL::java::INetworkStatsService::getMobileIfaces::server","com.android.phone","system_server","m.android.phone",1469,657,1,-800,-900,1736110278076,1736110435876,765487,462664,765487,462664,1736110692464,135281,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getIfaceStats::server","com.android.phone","system_server","m.android.phone",1469,657,1,-800,-900,1736111274404,1736111340019,481038,361607,481038,361607,1736111417370,249758,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getMobileIfaces::server","com.android.phone","system_server","m.android.phone",1469,657,1,-800,-900,1736111874030,1736111923740,254494,159330,254494,159330,1736111994038,64535,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getIfaceStats::server","com.android.phone","system_server","m.android.phone",1469,657,1,-800,-900,1736112257185,1736112301639,309870,220751,309870,220751,1736112361927,133727,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::IPackageManager::isProtectedBroadcast::server","com.android.systemui","system_server","ndroid.systemui",1253,657,1,-800,-900,1737108493015,1737125387579,17949987,163732,17949987,163732,1737125511194,24959,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::IActivityManager::checkPermission::server","com.android.phone","system_server","m.android.phone",1469,2721,1,-800,-900,1737110161286,1737110746980,12677155,147315,12677155,147315,1737110799860,75563,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getMobileIfaces::server","com.android.phone","system_server","m.android.phone",1469,2721,1,-800,-900,1737123460104,1737123475761,447621,137704,447621,137704,1737123532124,48775,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getIfaceStats::server","com.android.phone","system_server","m.android.phone",1469,2721,1,-800,-900,1737123982140,1737123994640,191006,164185,191006,164185,1737124033555,109797,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getMobileIfaces::server","com.android.phone","system_server","m.android.phone",1469,2721,1,-800,-900,1737124228451,1737124238356,88522,66721,88522,66721,1737124269922,24911,1,33,"[NULL]",0,"[NULL]"
        "AIDL::java::INetworkStatsService::getIfaceStats::server","com.android.phone","system_server","m.android.phone",1469,2721,1,-800,-900,1737124369273,1737124378273,957260,95254,957260,95254,1737124406331,54810,1,33,"[NULL]",0,"[NULL]"
        """))

  def test_binder_outgoing_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
        SELECT HEX(pprof) FROM ANDROID_BINDER_OUTGOING_GRAPH(259)
      """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
        Sample:
        Values: 0
        Stack:
        /system/bin/surfaceflinger (0x0)
        binder:446_1 (0x0)

        Sample:
        Values: 0
        Stack:
        stealReceiveChannel (0x0)
        IDisplayEventConnection (0x0)
        /system/bin/surfaceflinger (0x0)
        binder:446_1 (0x0)
        """))

  def test_binder_incoming_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
        SELECT HEX(pprof) FROM ANDROID_BINDER_INCOMING_GRAPH(296)
      """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
        Sample:
        Values: 1764197
        Stack:
        fixupAppData (0x0)
        IInstalld (0x0)
        system_server (0x0)

        Sample:
        Values: 202423
        Stack:
        rmdex (0x0)
        IInstalld (0x0)
        system_server (0x0)

        Sample:
        Values: 438512
        Stack:
        cleanupInvalidPackageDirs (0x0)
        IInstalld (0x0)
        system_server (0x0)

        Sample:
        Values: 4734897
        Stack:
        invalidateMounts (0x0)
        IInstalld (0x0)
        system_server (0x0)

        Sample:
        Values: 7448312
        Stack:
        prepareAppProfile (0x0)
        IInstalld (0x0)
        system_server (0x0)

        Sample:
        Values: 91238713
        Stack:
        createAppDataBatched (0x0)
        IInstalld (0x0)
        system_server (0x0)
        """))

  def test_binder_graph_invalid_oom(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
        SELECT HEX(pprof) FROM ANDROID_BINDER_GRAPH(2000, 2000, 2000, 2000)
      """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
        """))

  def test_binder_graph_valid_oom(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder;
        SELECT HEX(pprof) FROM ANDROID_BINDER_GRAPH(-1000, 1000, -1000, 1000)
      """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
            Sample:
            Values: 0
            Stack:
            /system/bin/apexd (0x0)
            /system/bin/servicemanager (0x0)

            Sample:
            Values: 0
            Stack:
            /system/bin/bootanimation (0x0)
            /system/bin/surfaceflinger (0x0)

            Sample:
            Values: 0
            Stack:
            /system/bin/cameraserver (0x0)
            system_server (0x0)

            Sample:
            Values: 0
            Stack:
            /system/bin/storaged (0x0)
            /vendor/bin/hw/android.hardware.health-service.cuttlefish (0x0)

            Sample:
            Values: 0
            Stack:
            /system/bin/surfaceflinger (0x0)
            /system/bin/bootanimation (0x0)

            Sample:
            Values: 0
            Stack:
            /system/bin/surfaceflinger (0x0)
            /vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu (0x0)

            Sample:
            Values: 0
            Stack:
            media.metrics (0x0)
            /system/bin/audioserver (0x0)

            Sample:
            Values: 0
            Stack:
            system_server (0x0)
            /system/bin/servicemanager (0x0)

            Sample:
            Values: 0
            Stack:
            system_server (0x0)
            /system/bin/surfaceflinger (0x0)

            Sample:
            Values: 1004933
            Stack:
            /vendor/bin/hw/android.hardware.sensors-service.example (0x0)
            system_server (0x0)

            Sample:
            Values: 105827054
            Stack:
            /system/bin/installd (0x0)
            system_server (0x0)

            Sample:
            Values: 11316
            Stack:
            system_server (0x0)
            /apex/com.android.os.statsd/bin/statsd (0x0)

            Sample:
            Values: 12567639
            Stack:
            /system/bin/servicemanager (0x0)
            system_server (0x0)

            Sample:
            Values: 137623
            Stack:
            /vendor/bin/hw/android.hardware.lights-service.example (0x0)
            system_server (0x0)

            Sample:
            Values: 140719
            Stack:
            system_server (0x0)
            /system/bin/storaged (0x0)

            Sample:
            Values: 150044
            Stack:
            /vendor/bin/hw/android.hardware.input.processor-service.example (0x0)
            system_server (0x0)

            Sample:
            Values: 1877718
            Stack:
            /system/bin/surfaceflinger (0x0)
            system_server (0x0)

            Sample:
            Values: 19303
            Stack:
            system_server (0x0)
            /vendor/bin/hw/android.hardware.sensors-service.example (0x0)

            Sample:
            Values: 210889
            Stack:
            /system/bin/servicemanager (0x0)
            /apex/com.android.os.statsd/bin/statsd (0x0)

            Sample:
            Values: 21505514
            Stack:
            /system/bin/idmap2d (0x0)
            system_server (0x0)

            Sample:
            Values: 2221699
            Stack:
            /vendor/bin/hw/android.hardware.health-service.cuttlefish (0x0)
            system_server (0x0)

            Sample:
            Values: 25394
            Stack:
            /system/bin/servicemanager (0x0)
            /system/bin/surfaceflinger (0x0)

            Sample:
            Values: 2552696
            Stack:
            /system/bin/hwservicemanager (0x0)
            /system/bin/cameraserver (0x0)

            Sample:
            Values: 28045
            Stack:
            /apex/com.android.os.statsd/bin/statsd (0x0)
            system_server (0x0)

            Sample:
            Values: 297647
            Stack:
            /system/bin/hwservicemanager (0x0)
            system_server (0x0)

            Sample:
            Values: 3483649
            Stack:
            system_server (0x0)
            /system/bin/audioserver (0x0)

            Sample:
            Values: 3677545
            Stack:
            /system/bin/servicemanager (0x0)
            /system/bin/audioserver (0x0)

            Sample:
            Values: 3991341
            Stack:
            /system/bin/servicemanager (0x0)
            /system/bin/cameraserver (0x0)

            Sample:
            Values: 41164
            Stack:
            system_server (0x0)
            /vendor/bin/hw/android.hardware.health-service.cuttlefish (0x0)

            Sample:
            Values: 4948091
            Stack:
            system_server (0x0)
            /system/bin/cameraserver (0x0)

            Sample:
            Values: 629626
            Stack:
            /apex/com.android.hardware.vibrator/bin/hw/android.hardware.vibrator-service.example (0x0)
            system_server (0x0)

            Sample:
            Values: 78428525
            Stack:
            /vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu (0x0)
            /system/bin/surfaceflinger (0x0)

            Sample:
            Values: 81216
            Stack:
            /system/bin/vold (0x0)
            system_server (0x0)

            Sample:
            Values: 837989
            Stack:
            /system/bin/servicemanager (0x0)
            /system/bin/storaged (0x0)
        """))

  def test_android_dvfs_counters(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 200001000000
                pid: 2
                clock_set_rate {
                  name : "domain@1"
                  state: 400000
                }
              }
              event {
                timestamp: 200003000000
                pid: 2
                clock_set_rate {
                  name: "domain@1"
                  state: 1024000
                }
              }
              event {
                timestamp: 200005000000
                pid: 2
                clock_set_rate {
                  name: "domain@1"
                  state: 1024000
                }
              }
            }
            trusted_uid: 9999
            trusted_packet_sequence_id: 2
          }
         """),
        query="""
         INCLUDE PERFETTO MODULE android.dvfs;
         SELECT * FROM android_dvfs_counters;
         """,
        out=Csv("""
         "name","ts","value","dur"
         "domain@1 Frequency",200001000000,400000.000000,2000000
         "domain@1 Frequency",200003000000,1024000.000000,2000000
         "domain@1 Frequency",200005000000,1024000.000000,0
         """))

  def test_android_dvfs_counter_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 200001000000
                pid: 2
                clock_set_rate {
                  name : "domain@1"
                  state: 400000
                }
              }
              event {
                timestamp: 200001000000
                pid: 2
                clock_set_rate {
                name : "bus_throughput"
                state: 1014000
                }
              }
              event {
                timestamp: 200003000000
                pid: 2
                clock_set_rate {
                  name: "domain@1"
                  state: 1024000
                }
              }
              event {
                timestamp: 200003000000
                pid: 2
                clock_set_rate {
                  name: "bus_throughput"
                  state: 553000
                }
              }
              event {
                timestamp: 200005000000
                pid: 2
                clock_set_rate {
                  name: "domain@1"
                  state: 1024000
                }
              }
              event {
                timestamp: 200005000000
                pid: 527
                clock_set_rate {
                  name: "bus_throughput"
                  state: 553000
                }
              }
            }
            trusted_uid: 9999
            trusted_packet_sequence_id: 2
          }
         """),
        query="""
         INCLUDE PERFETTO MODULE android.dvfs;
         SELECT * FROM android_dvfs_counter_stats;
         """,
        out=Csv("""
         "name","max","min","dur","wgt_avg"
         "bus_throughput Frequency",1014000.000000,553000.000000,2000000,783500.000000
         "domain@1 Frequency",1024000.000000,400000.000000,2000000,712000.000000
         """))

  def test_android_dvfs_counter_residency(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 200001000000
                pid: 2
                clock_set_rate {
                name : "bus_throughput"
                state: 1014000
                }
              }
              event {
                timestamp: 200003000000
                pid: 2
                clock_set_rate {
                  name: "bus_throughput"
                  state: 553000
                }
              }
              event {
                timestamp: 200005000000
                pid: 527
                clock_set_rate {
                  name: "bus_throughput"
                  state: 553000
                }
              }
            }
            trusted_uid: 9999
            trusted_packet_sequence_id: 2
          }
         """),
        query="""
         INCLUDE PERFETTO MODULE android.dvfs;
         SELECT * FROM android_dvfs_counter_residency;
         """,
        out=Csv("""
         "name","value","dur","pct"
         "bus_throughput Frequency",553000.000000,2000000,50.000000
         "bus_throughput Frequency",1014000.000000,2000000,50.000000
         """))

  def test_app_process_starts(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.app_process_starts;
        SELECT
        process_name,
        pid,
        intent,
        reason,
        proc_start_ts,
        proc_start_dur,
        bind_app_ts,
        bind_app_dur,
        intent_ts,
        intent_dur,
        total_dur
        FROM android_app_process_starts
        ORDER BY proc_start_ts
      """,
        out=Csv("""
        "process_name","pid","intent","reason","proc_start_ts","proc_start_dur","bind_app_ts","bind_app_dur","intent_ts","intent_dur","total_dur"
        "com.android.providers.media.module",3487,"com.android.providers.media.fuse.ExternalStorageServiceImpl","service",1737343157905,6527831,1737386174098,156129409,1737542356088,2114114,201312297
        "com.android.externalstorage",3549," android.os.storage.action.VOLUME_STATE_CHANGED","broadcast",1739987238947,9277039,1740045665263,20602351,1740066288912,1480586,80530551
      """))

  def test_garbage_collection(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.garbage_collection;
        SELECT
        tid,
        pid,
        thread_name,
        process_name,
        gc_type,
        is_mark_compact,
        reclaimed_mb,
        min_heap_mb,
        max_heap_mb
        gc_ts,
        gc_dur,
        gc_running_dur,
        gc_runnable_dur,
        gc_unint_io_dur,
        gc_unint_non_io_dur,
        gc_int_dur
        FROM android_garbage_collection_events
        ORDER BY tid, gc_ts
      """,
        out=Csv("""
        "tid","pid","thread_name","process_name","gc_type","is_mark_compact","reclaimed_mb","min_heap_mb","gc_ts","gc_dur","gc_running_dur","gc_runnable_dur","gc_unint_io_dur","gc_unint_non_io_dur","gc_int_dur"
        2013,2003,"HeapTaskDaemon","android.process.media","collector_transition",0,0.670000,2.153000,2.823000,326468170,80326441,11087787,0,0,10056086
        3494,3487,"HeapTaskDaemon","com.android.providers.media.module","young",0,"[NULL]","[NULL]","[NULL]",213263593,55205035,10429437,0,0,1208604
        3494,3487,"HeapTaskDaemon","com.android.providers.media.module","collector_transition",0,1.248000,2.201000,3.449000,169735717,65828710,20965673,0,0,0
        3556,3549,"HeapTaskDaemon","com.android.externalstorage","collector_transition",0,0.450000,2.038000,2.488000,166379142,52906367,7881722,0,0,0
        """))

  def test_input_events(self):
    return DiffTestBlueprint(
        trace=DataPath('post_boot_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
        total_latency_dur,
        handling_latency_dur,
        dispatch_latency_dur,
        tid,
        thread_name,
        pid,
        process_name,
        event_type,
        event_seq,
        event_channel,
        dispatch_ts,
        dispatch_dur,
        receive_ts,
        receive_dur
        FROM android_input_events
        ORDER BY dispatch_ts
        LIMIT 10
      """,
        out=Csv("""
        "total_latency_dur","handling_latency_dur","dispatch_latency_dur","tid","thread_name","pid","process_name","event_type","event_seq","event_channel","dispatch_ts","dispatch_dur","receive_ts","receive_dur"
        377149054,77503,377032734,7493,"ndroid.systemui",7493,"com.android.systemui","0x3","0x1","4325794 NotificationShade (server)",578307771330,1292,578684804064,1412
        1684318,772908,48433,7493,"ndroid.systemui",7493,"com.android.systemui","0x1","0x2","a0526ca NavigationBar0 (server)",581956322279,1299,581956370712,1806
        22069988,12614508,804831,7493,"ndroid.systemui",7493,"com.android.systemui","0x1","0x3","4325794 NotificationShade (server)",581956391308,1212,581957196139,1362
        1603522,645723,75328,7964,"droid.launcher3",7964,"com.android.launcher3","0x1","0x4","[Gesture Monitor] swipe-up (server)",581956445376,1232,581956520704,1708
        1583707,644313,208973,7310,"android.ui",7288,"system_server","0x1","0x5","PointerEventDispatcher0 (server)",581956495788,1208,581956704761,1281
        22622740,22582066,25729,7493,"ndroid.systemui",7493,"com.android.systemui","0x1","0x6","4325794 NotificationShade (server)",582019627670,1230,582019653399,1607
        20228399,20116160,95263,7964,"droid.launcher3",7964,"com.android.launcher3","0x1","0x7","[Gesture Monitor] swipe-up (server)",582019685639,1309,582019780902,1942
        459763,287436,27342,7310,"android.ui",7288,"system_server","0x1","0x8","PointerEventDispatcher0 (server)",582019737156,1192,582019764498,1664
        9848456,9806401,22714,7493,"ndroid.systemui",7493,"com.android.systemui","0x1","0x9","4325794 NotificationShade (server)",582051061377,1227,582051084091,1596
        5533919,5487703,25013,7964,"droid.launcher3",7964,"com.android.launcher3","0x1","0xa","[Gesture Monitor] swipe-up (server)",582051112236,1258,582051137249,1771
      """))

  def test_job_scheduler_events(self):
    return DiffTestBlueprint(
        trace=DataPath('post_boot_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.job_scheduler;
        SELECT job_id, uid, package_name, job_service_name, ts, dur FROM android_job_scheduler_events ORDER BY ts
      """,
        out=Csv("""
        "job_id","uid","package_name","job_service_name","ts","dur"
        237039804,1000,"android","com.android.server.notification.NotificationHistoryJobService$system",575488743679,10909825
        201,10060,"com.android.dialer","com.android.voicemail.impl.StatusCheckJobService",579210443477,15650722
        -300,10089,"com.android.providers.media.module","com.android.providers.media.MediaService",579448376938,1716731633
        7,10085,"com.android.devicelockcontroller","androidx.work.impl.background.systemjob.SystemJobService",579645356805,148784109
        2,10058,"com.android.imsserviceentitlement",".fcm.FcmRegistrationService",580025518616,47458225
        1000,10071,"com.android.messaging",".datamodel.action.ActionServiceImpl",581680366145,327541238
        1001,10071,"com.android.messaging",".datamodel.action.BackgroundWorkerService",581948976360,90502706
        1000,10071,"com.android.messaging",".datamodel.action.ActionServiceImpl",582038224048,65747884
        7,10088,"com.android.rkpdapp","androidx.work.impl.background.systemjob.SystemJobService",582582119592,103911382
        7,10037,"com.android.statementservice","androidx.work.impl.background.systemjob.SystemJobService",583151483122,115767494
        27950934,10022,"com.android.providers.calendar",".CalendarProviderJobService",587237955847,37434516
        """))

  def test_freezer_events(self):
    return DiffTestBlueprint(
        trace=DataPath('freezer_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.freezer;
        SELECT pid, ts, dur, unfreeze_reason_int, unfreeze_reason_str FROM android_freezer_events ORDER BY ts
      """,
        out=Csv("""
        "pid","ts","dur","unfreeze_reason_int","unfreeze_reason_str"
        6506,91266310231819,94699935803,"[NULL]","[NULL]"
        3804,91266322277324,94687890298,"[NULL]","[NULL]"
        3299,91281767065245,78699885147,6,"start_service"
        5782,91296291190245,64718977377,"[NULL]","[NULL]"
        6533,91296292403211,64717764411,"[NULL]","[NULL]"
        4044,91296293188372,64716979250,"[NULL]","[NULL]"
        4002,91296294215356,64715952266,"[NULL]","[NULL]"
        3981,91296294804650,64715362972,"[NULL]","[NULL]"
        """))

  def test_service_bindings(self):
    return DiffTestBlueprint(
        trace=DataPath('post_boot_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.services;
        SELECT
        client_oom_score,
        client_process,
        client_thread,
        client_pid,
        client_tid,
        client_ts,
        client_dur,
        server_oom_score,
        server_process,
        server_thread,
        server_tid,
        server_pid,
        server_ts,
        server_dur,
        token,
        act,
        cmp,
        flg,
        bind_seq
        FROM android_service_bindings
        ORDER BY client_tid, client_ts
        LIMIT 10
      """,
        out=Csv("""
        "client_oom_score","client_process","client_thread","client_pid","client_tid","client_ts","client_dur","server_oom_score","server_process","server_thread","server_tid","server_pid","server_ts","server_dur","token","act","cmp","flg","bind_seq"
        -900,"system_server","system_server",7288,7288,577830735575,0,0,"android.ext.services","binder:7732_3",7764,7732,577866081720,9755069,"android.os.BinderProxy@a0dc800","android.service.notification.NotificationAssistantService","android.ext.services/.notification.Assistant","[NULL]",21
        -900,"system_server","eduling.default",7288,7366,579204777498,0,0,"com.android.dialer","binder:8075_2",8097,8075,579207718770,13090141,"android.os.BinderProxy@9a28fdf","[NULL]","com.android.dialer/com.android.voicemail.impl.StatusCheckJobService","0x4",29
        -900,"system_server","eduling.default",7288,7366,580022869386,0,0,"com.android.imsserviceentitlement","binder:8647_1",8667,8647,580027477378,1982139,"android.os.BinderProxy@27f8e83","[NULL]","com.android.imsserviceentitlement/.fcm.FcmRegistrationService","0x4",35
        -900,"system_server","StorageManagerS",7288,7397,587754918358,0,-700,"com.android.providers.media.module","binder:8294_1",8327,8294,587757305854,2691423,"android.os.BinderProxy@73b68b5","[NULL]","com.android.providers.media.module/com.android.providers.media.fuse.ExternalStorageServiceImpl","[NULL]",44
        -800,"com.android.systemui","ndroid.systemui",7493,7493,572995972978,8071106,-800,"com.android.systemui","binder:7493_4",7682,7493,573131280194,17181314,"android.os.BinderProxy@1c2ac60","android.service.wallpaper.WallpaperService","com.android.systemui/.wallpapers.ImageWallpaper","[NULL]",14
        -800,"com.android.systemui","ndroid.systemui",7493,7493,572995972978,8071106,-800,"com.android.systemui","binder:7493_4",7682,7493,577000518511,6977972,"android.os.BinderProxy@b18137","[NULL]","com.android.systemui/.keyguard.KeyguardService","0x100",15
        -800,"com.android.networkstack.process","rkstack.process",7610,7610,571078334504,7552850,-800,"com.android.networkstack.process","binder:7610_1",7633,7610,571090652307,74610898,"android.os.BinderProxy@ee1090b","android.net.INetworkStackConnector","com.android.networkstack/com.android.server.NetworkStackService","[NULL]",2
        -800,"com.android.networkstack.process","rkstack.process",7610,7610,571078334504,7552850,-800,"com.android.networkstack.process","binder:7610_1",7633,7610,571489537275,1570460,"android.os.BinderProxy@a0dc800","android.net.ITetheringConnector","com.android.networkstack.tethering/.TetheringService","[NULL]",3
        0,"com.android.bluetooth","droid.bluetooth",7639,7639,571248973750,9874358,-700,"com.android.bluetooth","binder:7639_2",7672,7639,571871169647,6460322,"android.os.BinderProxy@7482132","android.bluetooth.IBluetooth","com.android.bluetooth/.btservice.AdapterService","[NULL]",4
        -700,"com.android.bluetooth","droid.bluetooth",7639,7639,572342110044,4874276,-700,"com.android.bluetooth","binder:7639_2",7672,7639,572466393291,1404185,"android.os.BinderProxy@ce5a6fc","android.media.browse.MediaBrowserService","com.android.bluetooth/.avrcpcontroller.BluetoothMediaBrowserService","[NULL]",10
      """))

  def test_oom_adjuster_transitions(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.oom_adjuster;
        SELECT
        ts,
        dur,
        score,
        bucket,
        process_name,
        oom_adj_ts,
        oom_adj_dur,
        oom_adj_thread_name,
        oom_adj_reason,
        oom_adj_trigger
        FROM android_oom_adj_intervals
        WHERE oom_adj_reason IS NOT NULL
        ORDER BY ts
        LIMIT 10
      """,
        out=Csv("""
        "ts","dur","score","bucket","process_name","oom_adj_ts","oom_adj_dur","oom_adj_thread_name","oom_adj_reason","oom_adj_trigger"
1737065264829,701108081,925,"cached","com.android.providers.calendar",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737066678827,3470211742,935,"cached","com.android.imsserviceentitlement",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737066873002,3470017567,945,"cached","com.android.carrierconfig",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737067058812,3469831757,955,"cached","com.android.messaging",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737067246975,699224817,955,"cached","android.process.acore",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737068421919,3468468650,965,"cached","com.android.shell",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737068599673,697908135,965,"cached","android.process.media",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737068933602,3467956967,975,"cached","com.android.gallery3d",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737069091010,3467799559,975,"cached","com.android.packageinstaller",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
1737069240534,3467650035,985,"cached","com.android.managedprovisioning",1737064421516,29484835,"binder:642_1","processEnd","IActivityManager#1598246212"
      """))

  def test_broadcast_minsdk_u(self):
    return DiffTestBlueprint(
        trace=DataPath('freezer_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.broadcasts;
        SELECT intent_action, process_name, pid, queue_id, ts, dur FROM _android_broadcasts_minsdk_u
        ORDER BY ts LIMIT 10
      """,
        out=Csv("""
        "intent_action","process_name","pid","queue_id","ts","dur"
        "android.os.action.POWER_SAVE_TEMP_WHITELIST_CHANGED","system",2519,0,91286297271477,221619
        "android.intent.action.TIME_TICK","com.android.systemui",2762,0,91295942589896,469216
        "android.intent.action.TIME_TICK","com.android.systemui",2762,0,91295943366025,313104
        "android.intent.action.TIME_TICK","com.android.systemui",2762,0,91295943943713,356194
        "android.intent.action.TIME_TICK","com.android.systemui",2762,0,91355941417856,444189
        "android.intent.action.TIME_TICK","com.android.systemui",2762,0,91355942543001,405369
        "android.intent.action.TIME_TICK","com.android.systemui",2762,0,91355943262781,339640
        "android.intent.action.PACKAGE_NEEDS_INTEGRITY_VERIFICATION","system",2519,0,91359865607938,862534
        "android.content.pm.action.SESSION_COMMITTED","com.android.launcher3",3219,0,91360380556725,15221753
        "android.intent.action.PACKAGE_ADDED","system",2519,0,91360396877398,107502
        """))


  def test_binder_breakdown(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.binder_breakdown;
        WITH x AS (
          SELECT reason, dur FROM android_binder_server_breakdown
          UNION ALL
          SELECT reason, dur FROM android_binder_client_breakdown
        ) SELECT reason, SUM(dur) AS dur FROM x GROUP BY reason ORDER BY dur
      """,
        out=Csv("""
        "reason","dur"
        "D",548774
        "io",705773
        "art_lock_contention",9500403
        "monitor_contention",76505897
        "R+",198506855
        "R",201261723
        "Running",608081756
        "binder",4174605447
        "S",5144384456
        """))
