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


class AndroidMetrics(TestSuite):

  def test_android_network_activity(self):
    # The following should have three activity regions:
    # * uid=123 from 1000 to 2010 (note: end is max(ts)+idle_ns)
    # * uid=456 from 1005 to 3115 (note: doesn't group with above due to name)
    #   * Also tests that groups form based on (ts+dur), not just start ts.
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
          timestamp: 1005
          network_packet_bundle {
            ctx {
              direction: DIR_EGRESS
              interface: "wlan"
              uid: 456
            }
            total_duration: 100
            total_packets: 2
            total_length: 300
          }
        }
        packet {
          timestamp: 2015
          network_packet_bundle {
            ctx {
              direction: DIR_EGRESS
              interface: "wlan"
              uid: 456
            }
            total_duration: 100
            total_packets: 1
            total_length: 50
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

  def test_anr_metric(self):
    return DiffTestBlueprint(
        trace=Path('android_anr_metric.py'),
        query=Metric('android_anr'),
        out=Path('android_anr_metric.out'))

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

  def test_android_sysui_notifications_blocking_calls(self):
    return DiffTestBlueprint(
        trace=Path('android_sysui_notifications_blocking_calls_metric.py'),
        query=Metric('android_sysui_notifications_blocking_calls_metric'),
        out=Path('android_sysui_notifications_blocking_calls_metric.out'))

  def test_monitor_contention_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query=Metric('android_monitor_contention'),
        out=Path('android_monitor_contention.out'))

  def test_monitor_contention_agg_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query=Metric('android_monitor_contention_agg'),
        out=TextProto(r"""
        android_monitor_contention_agg {
          process_aggregation {
            name: "android.process.media"
            total_contention_count: 12
            total_contention_dur: 12893198
            main_thread_contention_count: 12
            main_thread_contention_dur: 12893198
          }
          process_aggregation {
            name: "com.android.providers.media.module"
            total_contention_count: 7
            total_contention_dur: 169793
          }
          process_aggregation {
            name: "com.android.systemui"
            total_contention_count: 8
            total_contention_dur: 9445959
            main_thread_contention_count: 5
            main_thread_contention_dur: 9228582
          }
          process_aggregation {
            name: "system_server"
            total_contention_count: 354
            total_contention_dur: 358898613
            main_thread_contention_count: 27
            main_thread_contention_dur: 36904702
          }
        }
        """))

  def test_android_boot(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query=Metric('android_boot'),
        out=TextProto(r"""
        android_boot {
          system_server_durations {
            total_dur: 267193980530
            uninterruptible_sleep_dur: 3843119529
          }
        }
        """))

  def test_ad_services_metric(self):
    return DiffTestBlueprint(
        trace=Path('ad_services_metric.py'),
        query=Metric('ad_services_metric'),
        out=TextProto(r"""
         ad_services_metric {
           ui_metric {
             latency: 0.0003
           }
           app_set_id_metric {
             latency: 0.0001
           }
           ad_id_metric {
             latency:0.0003
           }
         }
        """))
