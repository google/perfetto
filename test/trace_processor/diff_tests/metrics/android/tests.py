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
              network_interface: "wlan"
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
              network_interface: "wlan"
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
              network_interface: "wlan"
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
              network_interface: "loopback"
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
        trace=DataPath('sched_wakeup_trace.atr'),
        query=Metric('android_binder'),
        out=Path('android_binder_metric.out'))

  def test_android_blocking_calls_cuj(self):
    return DiffTestBlueprint(
        trace=Path('android_blocking_calls_cuj_metric.py'),
        query=Metric('android_blocking_calls_cuj_metric'),
        out=Path('android_blocking_calls_cuj_metric.out'))

  def test_android_blocking_calls_unagg(self):
    return DiffTestBlueprint(
        trace=Path('android_blocking_calls_cuj_metric.py'),
        query=Metric('android_blocking_calls_unagg'),
        out=Path('android_blocking_calls_unagg.out'))

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

  def test_android_blocking_calls_cuj_different_ui_thread(self):
    return DiffTestBlueprint(
        trace=Path('android_blocking_calls_cuj_different_ui_thread.py'),
        query=Metric('android_blocking_calls_cuj_metric'),
        out=Path('android_blocking_calls_cuj_different_ui_thread.out'))

  def test_android_blocking_calls_cuj_per_frame(self):
    return DiffTestBlueprint(
        trace=Path('android_blocking_calls_cuj_per_frame_metric.py'),
        query=Metric('android_blocking_calls_cuj_per_frame_metric'),
        out=Path('android_blocking_calls_cuj_per_frame_metric.out'))

  def test_sysui_notif_shade_list_builder(self):
    return DiffTestBlueprint(
        trace=Path('android_sysui_notif_shade_list_builder_metric.py'),
        query=Metric('sysui_notif_shade_list_builder_metric'),
        out=Path('sysui_notif_shade_list_builder_metric.out'))

  def test_sysui_update_notif_on_ui_mode_changed(self):
    return DiffTestBlueprint(
        trace=Path('sysui_update_notif_on_ui_mode_changed_metric.py'),
        query=Metric('sysui_update_notif_on_ui_mode_changed_metric'),
        out=Path('sysui_update_notif_on_ui_mode_changed_metric.out'))

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
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric('android_boot'),
        out=Path('android_boot.out'))

  def test_ad_services_metric(self):
    return DiffTestBlueprint(
        trace=Path('ad_services_metric.py'),
        query=Metric('ad_services_metric'),
        out=TextProto(r"""
         ad_services_metric {
           ui_metric {
             consent_manager_initialization_latency: 0.0003
             consent_manager_read_latency: 0.00015
           }
           app_set_id_metric {
             latency: 0.0001
           }
           ad_id_metric {
             latency: 0.0003
           }
           odp_metric {
             managing_service_initialization_latency: 0.00005
             service_delegate_execute_flow_latency: 0.0001
             service_delegate_request_surface_package_latency: 0.00015
             service_delegate_register_web_trigger_latency: 0.0002
           }
         }
        """))

  def test_android_boot_unagg(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric("android_boot_unagg"),
        out=Path('android_boot_unagg.out'))

  def test_android_app_process_starts(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric("android_app_process_starts"),
        out=Path('android_app_process_starts.out'))

  def test_android_garbage_collection(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric('android_garbage_collection_unagg'),
        out=Path('android_garbage_collection_unagg.out'))

  def test_android_garbage_collection_stats(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric('android_garbage_collection_stats'),
        out=Path('android_garbage_collection_stats.out'))

  def test_android_auto_multiuser_switch(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 1000000000
              pid: 4032
              print {
                buf: "S|5993|UserController.startUser-10-fg-start-mode-1|0\n"
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 2000000000
              pid: 4065
              print {
                buf: "S|2608|launching: com.android.car.carlauncher|0\n"
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 3000000000
              pid: 4032
              print {
                buf: "S|5993|UserController.startUser-11-fg-start-mode-1|0\n"
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 6878000000
              pid: 4065
              print {
                buf: "S|2609|launching: com.android.car.carlauncher|0\n"
              }
            }
          }
        }
        """),
        query=Metric('android_auto_multiuser'),
        out=TextProto(r"""
       android_auto_multiuser {
         user_switch {
            user_id: 11
            start_event: "UserController.startUser-11-fg-start-mode-1"
            end_event: "com.android.car.carlauncher"
            duration_ms: 3877
            previous_user_info {
            }
         }
       }
       """))

  def test_android_auto_multiuser_switch_with_previous_user_data(self):
    return DiffTestBlueprint(
        trace=Path("android_auto_multiuser.textproto"),
        query=Metric('android_auto_multiuser'),
        out=TextProto(r"""
       android_auto_multiuser {
         user_switch {
            user_id: 11
            start_event: "UserController.startUser-11-fg-start-mode-1"
            end_event: "com.android.car.carlauncher"
            duration_ms: 999
            previous_user_info {
                user_id: 10
                total_cpu_time_ms: 9
                total_memory_usage_kb: 2048
            }
         }
          user_switch {
             user_id: 11
             start_event: "UserController.startUser-11-fg-start-mode-1"
             end_event: "finishUserStopped-10-[stopUser]"
             duration_ms: 2100
             previous_user_info {
                 user_id: 10
                 total_cpu_time_ms: 19
                 total_memory_usage_kb: 3072
             }
          }
       }
       """))

  def test_android_auto_multiuser_timing_table(self):
    return DiffTestBlueprint(
        trace=Path("android_auto_multiuser.textproto"),
        query="""
        INCLUDE PERFETTO MODULE android.auto.multiuser;
        SELECT * FROM android_auto_multiuser_timing;
        """,
        out=Csv("""
        "event_start_user_id","event_start_time","event_end_time","event_end_name","event_start_name","duration"
        "11",3000000000,3999999999,"com.android.car.carlauncher","UserController.startUser-11-fg-start-mode-1",999999999
        "11",3000000000,5100000000,"finishUserStopped-10-[stopUser]","UserController.startUser-11-fg-start-mode-1",2100000000
        """))

  def test_android_oom_adjuster(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric("android_oom_adjuster"),
        out=Path('android_oom_adjuster.out'))

  def test_android_broadcasts(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=Metric("android_broadcasts"),
        out=Path('android_broadcasts.out'))

  def test_wattson_app_startup_rails_output(self):
    return DiffTestBlueprint(
        trace=DataPath('android_calculator_startup.pb'),
        query=Metric("wattson_app_startup_rails"),
        out=Csv("""
        wattson_app_startup_rails {
          metric_version: 4
          power_model_version: 1
          is_crude_estimate: false
          period_info {
            period_id: 1
            period_dur: 385136434
            cpu_subsystem {
              estimated_mw: 4583.882324
              estimated_mws: 1765.420166
              policy0 {
                estimated_mw: 573.628662
                estimated_mws: 220.925293
                cpu0 {
                  estimated_mw: 147.795715
                  estimated_mws: 56.921513
                }
                cpu1 {
                  estimated_mw: 128.964539
                  estimated_mws: 49.668941
                }
                cpu2 {
                  estimated_mw: 126.593117
                  estimated_mws: 48.755619
                }
                cpu3 {
                  estimated_mw: 170.275269
                  estimated_mws: 65.579208
                }
              }
              policy4 {
                estimated_mw: 684.205200
                estimated_mws: 263.512360
                cpu4 {
                  estimated_mw: 344.398224
                  estimated_mws: 132.640305
                }
                cpu5 {
                  estimated_mw: 339.806946
                  estimated_mws: 130.872040
                }
              }
              policy6 {
                estimated_mw: 2163.280029
                estimated_mws: 833.157959
                cpu6 {
                  estimated_mw: 1080.785522
                  estimated_mws: 416.249878
                }
                cpu7 {
                  estimated_mw: 1082.494507
                  estimated_mws: 416.908051
                }
              }
              dsu_scu {
                estimated_mw: 1162.768677
                estimated_mws: 447.824585
              }
            }
          }
        }
        """))

  def test_wattson_estimate_output(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_eos_suspend.pb'),
        query=Metric("wattson_trace_rails"),
        out=Csv("""
        wattson_trace_rails {
          metric_version: 4
          power_model_version: 1
          is_crude_estimate: false
          period_info {
            period_id: 1
            period_dur: 61793079818
            cpu_subsystem {
              estimated_mw: 42.123875
              estimated_mws: 2602.963867
              policy0 {
                estimated_mw: 34.719143
                estimated_mws: 2145.402832
                cpu0 {
                  estimated_mw: 10.705154
                  estimated_mws: 661.504456
                }
                cpu1 {
                  estimated_mw: 8.315764
                  estimated_mws: 513.856689
                }
                cpu2 {
                  estimated_mw: 7.7776737
                  estimated_mws: 480.606415
                }
                cpu3 {
                  estimated_mw: 7.9205513
                  estimated_mws: 489.435272
                }
              }
              dsu_scu {
                estimated_mw: 7.4047313
                estimated_mws: 457.561157
              }
            }
          }
        }
        """))

  def test_wattson_trace_rails_wo_cpuidle(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_tk4_aot.pb'),
        query=Metric("wattson_trace_rails"),
        out=Csv("""
        wattson_trace_rails {
          metric_version: 4
          power_model_version: 1
          is_crude_estimate: true
          period_info {
            period_id: 1
            period_dur: 16532191699
            cpu_subsystem {
              estimated_mw: 94.580833
              estimated_mws: 1563.628418
              policy0 {
                estimated_mw: 48.416279
                estimated_mws: 800.427185
                cpu0 {
                  estimated_mw: 11.371411
                  estimated_mws: 187.994354
                }
                cpu1 {
                  estimated_mw: 12.886018
                  estimated_mws: 213.034119
                }
                cpu2 {
                  estimated_mw: 12.652553
                  estimated_mws: 209.174423
                }
                cpu3 {
                  estimated_mw: 11.506296
                  estimated_mws: 190.224289
                }
              }
              policy4 {
                estimated_mw: 22.661949
                estimated_mws: 374.651703
                cpu4 {
                  estimated_mw: 9.433892
                  estimated_mws: 155.962921
                }
                cpu5 {
                  estimated_mw: 6.5253134
                  estimated_mws: 107.877731
                }
                cpu6 {
                  estimated_mw: 6.702744
                  estimated_mws: 110.811043
                }
              }
              policy7 {
                estimated_mw: 19.993582
                estimated_mws: 330.537750
                cpu7 {
                  estimated_mw: 19.993582
                  estimated_mws: 330.537750
                }
              }
              dsu_scu {
                estimated_mw: 3.509021
                estimated_mws: 58.011806
              }
            }
          }
        }
        """))

  def test_wattson_trace_threads_output(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=Metric("wattson_trace_threads"),
        out=Path('wattson_trace_threads.out'))

  def test_anomaly_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('android_binder_metric_trace.atr'),
        query=Metric('android_anomaly'),
        out=Path('android_anomaly_metric.out'))

  def test_wattson_markers_threads_output(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_w_packages_Imarkers.pb'),
        query=Metric("wattson_markers_threads"),
        out=Path('wattson_markers_threads.out'))

  def test_wattson_markers_rails_output(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_w_packages_Imarkers.pb'),
        query=Metric("wattson_markers_rails"),
        out=Csv("""
        wattson_markers_rails {
          metric_version: 4
          power_model_version: 1
          is_crude_estimate: false
          period_info {
            period_id: 1
            period_dur: 2031888905
            cpu_subsystem {
              estimated_mw: 46.541107
              estimated_mws: 94.566360
              policy0 {
                estimated_mw: 34.037655
                estimated_mws: 69.160736
                cpu0 {
                  estimated_mw: 14.416669
                  estimated_mws: 29.293070
                }
                cpu1 {
                  estimated_mw: 6.641506
                  estimated_mws: 13.494802
                }
                cpu2 {
                  estimated_mw: 8.134805
                  estimated_mws: 16.529020
                }
                cpu3 {
                  estimated_mw: 4.844675
                  estimated_mws: 9.843842
                }
              }
              dsu_scu {
                estimated_mw: 12.503453
                estimated_mws: 25.405626
              }
            }
          }
        }
        """))

  def test_wattson_atrace_apps_rails_output(self):
    return DiffTestBlueprint(
        trace=DataPath('sysui_qsmedia_microbenchmark.pb'),
        query=Metric("wattson_atrace_apps_rails"),
        out=Path('wattson_atrace_apps_rails.out'))

  def test_wattson_atrace_apps_threads_output(self):
    return DiffTestBlueprint(
        trace=DataPath('sysui_qsmedia_microbenchmark.pb'),
        query=Metric("wattson_atrace_apps_threads"),
        out=Path('wattson_atrace_apps_threads.out'))

  def test_wattson_app_startup_threads_output(self):
    return DiffTestBlueprint(
        trace=DataPath('android_calculator_startup.pb'),
        query=Metric("wattson_app_startup_threads"),
        out=Path('wattson_app_startup_threads.out'))
