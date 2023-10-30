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
        out=Path('android_battery_stats_state.out'))

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
        FROM android_sync_binder_metrics_by_txn
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
        ORDER BY name;
        """,
        out=Path('android_slice_standardization.out'))

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
        "parent_id","blocking_method","blocked_method","short_blocking_method","short_blocked_method","blocking_src","blocked_src","waiter_count","blocked_utid","blocked_thread_name","blocking_utid","blocking_thread_name","blocking_tid","upid","process_name","id","ts","dur","track_id","is_blocked_thread_main","blocked_thread_tid","is_blocking_thread_main","blocking_thread_tid","binder_reply_id","binder_reply_ts","binder_reply_tid","pid","child_id"
        949,"void com.android.server.am.ActivityManagerService$AppDeathRecipient.binderDied()","int com.android.server.am.ActivityManagerService.getMemoryTrimLevel()","com.android.server.am.ActivityManagerService$AppDeathRecipient.binderDied","com.android.server.am.ActivityManagerService.getMemoryTrimLevel","ActivityManagerService.java:1478","ActivityManagerService.java:9183",1,250,"system_server",656,"binder:642_12",2720,250,"system_server",956,1737123891932,17577143,1215,1,642,0,2720,"[NULL]","[NULL]","[NULL]",642,"[NULL]"
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
      SELECT * FROM ANDROID_THREAD_CREATION_SPAM(1e9, 1e9);
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
      SELECT * FROM android_io_f2fs_counter_stats;
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
      SELECT tid, thread_name, pid, process_name, ino, dev, bytes, write_count FROM android_io_f2fs_write_stats;
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
               distinct_device_count, distict_inode_count, distinct_thread_count
        FROM android_io_f2fs_aggregate_write_stats
        """,
        out=Csv("""
        "total_write_count","distinct_processes","total_bytes_written","distinct_device_count","distict_inode_count","distinct_thread_count"
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
          server_ts
        FROM android_async_binder_metrics_by_txn
        WHERE aidl_name IS NOT NULL
        ORDER BY client_ts
        LIMIT 10;
      """,
        out=Csv("""
        "aidl_name","client_process","server_process","client_thread","client_tid","server_tid","is_main_thread","client_oom_score","server_oom_score","client_ts","server_ts"
        "AIDL::cpp::IClientCallback::onClients::cppServer","/system/bin/servicemanager","/system/bin/apexd","servicemanager",243,386,1,-1000,-1000,22213481492,22213517474
        "AIDL::cpp::IMediaMetricsService::submitBuffer::cppServer","/system/bin/audioserver","media.metrics","audioserver",492,1262,1,-1000,-1000,25512325446,25512488255
        "AIDL::cpp::IMediaMetricsService::submitBuffer::cppServer","/system/bin/audioserver","media.metrics","audioserver",492,1262,1,-1000,-1000,25512842465,25522410505
        "AIDL::cpp::IDisplayEventConnection::stealReceiveChannel::cppServer","/vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu","/system/bin/surfaceflinger","binder:446_1",553,522,0,-1000,-1000,25847718645,25847734867
        "AIDL::cpp::ITunnelModeEnabledListener::onTunnelModeEnabledChanged::cppServer","/system/bin/surfaceflinger","system_server","binder:496_2",522,1600,0,-1000,-900,25854181504,25854195485
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,522,0,-900,-1000,25855697394,25855710732
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,522,0,-900,-1000,25873210999,25873224961
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,522,0,-900,-1000,25951278287,25952242397
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,1575,0,-900,-1000,25965452828,25965590137
        "AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppServer","system_server","/system/bin/surfaceflinger","android.anim",662,1575,0,-900,-1000,26046376252,26046544680
      """))

  def test_binder_txns(self):
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
          server_ts
        FROM android_binder_txns
        WHERE aidl_name IS NOT NULL
        ORDER BY client_ts
        LIMIT 10;
      """,
        out=Csv("""
        "aidl_name","client_process","server_process","client_thread","client_tid","server_tid","is_main_thread","client_oom_score","server_oom_score","client_ts","server_ts"
        "AIDL::cpp::IClientCallback::onClients::cppServer","/system/bin/servicemanager","/system/bin/apexd","servicemanager",243,386,1,-1000,-1000,22213481492,22213517474
        "AIDL::cpp::IInstalld::rmdex::cppServer","system_server","/system/bin/installd","system_server",641,565,1,-1000,-1000,25230101202,25230125660
        "AIDL::cpp::IInstalld::cleanupInvalidPackageDirs::cppServer","system_server","/system/bin/installd","system_server",641,565,1,-1000,-1000,25243511980,25243544499
        "AIDL::cpp::IInstalld::createAppDataBatched::cppServer","system_server","/system/bin/installd","system_server",641,565,1,-1000,-1000,25244949065,25244971300
        "AIDL::cpp::IInstalld::prepareAppProfile::cppServer","system_server","/system/bin/installd","system_server",641,565,1,-1000,-1000,25279371214,25279387389
        "AIDL::cpp::IInstalld::prepareAppProfile::cppServer","system_server","/system/bin/installd","system_server",641,548,1,-1000,-1000,25279567724,25279592927
        "AIDL::cpp::IInstalld::prepareAppProfile::cppServer","system_server","/system/bin/installd","system_server",641,548,1,-1000,-1000,25280736368,25280756522
        "AIDL::cpp::IInstalld::prepareAppProfile::cppServer","system_server","/system/bin/installd","system_server",641,548,1,-1000,-1000,25280932813,25280946041
        "AIDL::cpp::IInstalld::prepareAppProfile::cppServer","system_server","/system/bin/installd","system_server",641,548,1,-1000,-1000,25281131360,25281145719
        "AIDL::cpp::IInstalld::prepareAppProfile::cppServer","system_server","/system/bin/installd","system_server",641,548,1,-1000,-1000,25281273755,25281315273
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
         "domain@1 Frequency",200005000000,1024000.000000,1
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
         "bus_throughput Frequency",1014000.000000,553000.000000,4000000,783499.942375
         "domain@1 Frequency",1024000.000000,400000.000000,4000000,712000.078000
         """))

  def test_android_dvfs_counter_residency(self):
      return DiffTestBlueprint(
          trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 200001000001
                pid: 2
                clock_set_rate {
                name : "bus_throughput"
                state: 1014000
                }
              }
              event {
                timestamp: 200003000001
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
