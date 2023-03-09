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
        SELECT t.id, t.type, t.name, c.id, c.ts, c.type, c.value
        FROM counter_track t JOIN counter c ON t.id = c.track_id
        WHERE name = 'ScreenState';
        """,
        out=Csv("""
        "id","type","name","id","ts","type","value"
        0,"counter_track","ScreenState",0,1000,"counter",2.000000
        0,"counter_track","ScreenState",1,2000,"counter",1.000000
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
        SELECT t.id, t.type, t.name, s.id, s.ts, s.dur, s.type, s.name
        FROM track t JOIN slice s ON s.track_id = t.id
        WHERE t.name = 'DeviceStateChanged';
        """,
        out=Path('android_system_property_slice.out'))

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
          is_main_thread,
          client_ts,
          client_dur,
          binder_reply_id,
          server_process,
          server_thread,
          server_upid,
          server_utid,
          server_ts,
          server_dur
        FROM android_sync_binder_metrics_by_txn
        WHERE binder_txn_id = 34382
        ORDER BY client_ts
        LIMIT 1;
      """,
        out=Csv("""
      "aidl_name","binder_txn_id","client_process","client_thread","client_upid","client_utid","is_main_thread","client_ts","client_dur","binder_reply_id","server_process","server_thread","server_upid","server_utid","server_ts","server_dur"
      "AIDL::java::ISensorPrivacyManager::isSensorPrivacyEnabled::server",34382,"/system/bin/audioserver","audioserver",281,281,1,25505818197,3125407,34383,"system_server","binder:641_4",311,539,25505891588,3000749
      """))

  def test_binder_sync_binder_thread_state(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
      SELECT IMPORT('android.binder');
      SELECT
        binder_txn_id,
        binder_reply_id,
        thread_state_type,
        thread_state,
        thread_state_dur,
        thread_state_count
      FROM android_sync_binder_thread_state_by_txn
      WHERE binder_txn_id = 34382
      ORDER BY thread_state_dur;
      """,
        out=Csv("""
      "binder_txn_id","binder_reply_id","thread_state_type","thread_state","thread_state_dur","thread_state_count"
      34382,34383,"binder_reply","R+",10030,1
      34382,34383,"binder_txn","Running",26597,2
      34382,34383,"binder_txn","R",38947,1
      34382,34383,"binder_reply","Running",533663,3
      34382,34383,"binder_reply","D",864664,1
      34382,34383,"binder_reply","R",1592392,1
      34382,34383,"binder_txn","S",3059863,1
      """))

  def test_binder_sync_binder_blocked_function(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query="""
      SELECT IMPORT('android.binder');
      SELECT
        binder_txn_id,
        binder_reply_id,
        thread_state_type,
        blocked_function,
        blocked_function_dur,
        blocked_function_count
      FROM android_sync_binder_blocked_functions_by_txn
      WHERE binder_txn_id = 34382
      ORDER BY blocked_function_dur;
      """,
        out=Csv("""
      "binder_txn_id","binder_reply_id","thread_state_type","blocked_function","blocked_function_dur","blocked_function_count"
      34382,34383,"binder_reply","filemap_fault",864664,1
      """))

  def test_binder_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query=Metric('android_binder'),
        out=Path('android_binder_metric.out'))
