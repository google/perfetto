#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, Path, DataPath, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class WattsonStdlib(TestSuite):
  # Test that the device name can be extracted from the trace's metadata.
  def test_wattson_device_name(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_wo_device_name.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.device_infos;
            select name from _wattson_device
            """),
        out=Csv("""
            "name"
            "monaco"
            """))

  # Test that Wattson device is resolved from Linux devicetree compatible metadata.
  def test_wattson_linux_device_name(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.1.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "google,gs101-oriole"
            device_tree_compatibles: "google,gs101"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE wattson.device_infos;
        SELECT name FROM _wattson_device;
        """,
        out=Csv("""
        "name"
        "Tensor"
        """))

  # Test that Wattson device is resolved from SoC compatible when board is unknown.
  def test_wattson_linux_device_soc_fallback(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.1.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "vendor,unknown-board"
            device_tree_compatibles: "google,gs101"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE wattson.device_infos;
        SELECT name FROM _wattson_device;
        """,
        out=Csv("""
        "name"
        "Tensor"
        """))

  # Test that Wattson device is resolved from board compatible fallback.
  def test_wattson_linux_device_board_fallback(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.1.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "google,GS101 BLUEJAY"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE wattson.device_infos;
        SELECT name FROM _wattson_device;
        """,
        out=Csv("""
        "name"
        "Tensor"
        """))

  # Test that Wattson device ignores devicetree compatibles from remote machines.
  def test_wattson_linux_device_multi_machine(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.1.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "google,gs101"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        packet {
          machine_id: 1001
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.6.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "qcom,sm8750-mtp"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 2
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE wattson.device_infos;
        SELECT name FROM _wattson_device;
        """,
        out=Csv("""
        "name"
        "Tensor"
        """))

  # Tests intermediate table
  def test_wattson_intermediate_table(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
              select
                ts,dur,l3_hit_count,l3_miss_count,freq_0,idle_0,freq_1,idle_1,freq_2,idle_2,freq_3,idle_3,freq_4,idle_4,freq_5,idle_5,freq_6,idle_6,freq_7,idle_7,cpu4_curve,cpu5_curve,cpu6_curve,cpu7_curve
              from _w_independent_cpus_calc
              WHERE ts > 359661672577
              ORDER by ts ASC
              LIMIT 10
            """),
        out=Csv("""
          "ts","dur","l3_hit_count","l3_miss_count","freq_0","idle_0","freq_1","idle_1","freq_2","idle_2","freq_3","idle_3","freq_4","idle_4","freq_5","idle_5","freq_6","idle_6","freq_7","idle_7","cpu4_curve","cpu5_curve","cpu6_curve","cpu7_curve"
          359661672578,75521,8326,9689,1401000,0,1401000,0,1401000,0,1401000,0,2253000,-1,2253000,0,2802000,-1,2802000,0,527.050000,23.500000,1942.890000,121.430000
          359661748099,2254517,248577,289258,1401000,0,1401000,0,1401000,0,1401000,0,2253000,0,2253000,0,2802000,-1,2802000,0,23.500000,23.500000,1942.890000,121.430000
          359664002616,81,8,10,1401000,0,1401000,0,1401000,0,1401000,0,2253000,0,2253000,0,2802000,-1,2802000,-1,23.500000,23.500000,1942.890000,1942.890000
          359664002697,488,53,62,1401000,0,1401000,0,1401000,0,1401000,0,2253000,-1,2253000,-1,2802000,-1,2802000,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003185,122,13,15,1401000,-1,1401000,0,1401000,0,1401000,0,2253000,-1,2253000,-1,2802000,-1,2802000,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003307,163,17,20,1401000,-1,1401000,0,1401000,-1,1401000,0,2253000,-1,2253000,-1,2802000,-1,2802000,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003470,204,22,26,1401000,-1,1401000,0,1401000,-1,1401000,-1,2253000,-1,2253000,-1,2802000,-1,2802000,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003674,11596,1278,1487,1401000,-1,1401000,-1,1401000,-1,1401000,-1,2253000,-1,2253000,-1,2802000,-1,2802000,-1,527.050000,527.050000,1942.890000,1942.890000
          359664015270,4720,520,605,1401000,-1,1401000,-1,1401000,-1,1401000,-1,2253000,-1,2253000,-1,2802000,-1,2802000,0,527.050000,527.050000,1942.890000,121.430000
          359664019990,18921,2086,2427,1401000,-1,1401000,-1,1401000,-1,1401000,-1,2253000,0,2253000,-1,2802000,-1,2802000,0,23.500000,527.050000,1942.890000,121.430000
            """))

  # Tests that device static curve selection is only when CPUs are active
  def test_wattson_static_curve_selection(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
              SELECT
                ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                cpu6_mw, cpu7_mw, dsu_scu_mw
              FROM _system_state_mw
              ORDER by ts ASC
              LIMIT 5
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            359085634940,569,0.000000,"[NULL]","[NULL]","[NULL]",0.000000,28.510000,"[NULL]","[NULL]",0.000000
            359085635509,733,0.000000,0.000000,"[NULL]","[NULL]",0.000000,28.510000,"[NULL]","[NULL]",0.000000
            359085636242,651,0.000000,0.000000,0.000000,"[NULL]",0.000000,28.510000,"[NULL]","[NULL]",0.000000
            359085636893,23030,0.000000,0.000000,0.000000,0.000000,0.000000,28.510000,"[NULL]","[NULL]",0.000000
            359085659923,6664673,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,"[NULL]","[NULL]",0.000000
            """))

  # Tests that L3 cache calculations are being done correctly
  def test_wattson_l3_calculations(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
              SELECT
                ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                cpu6_mw, cpu7_mw, dsu_scu_mw
              FROM _system_state_mw
              WHERE ts > 359661672577
              ORDER by ts ASC
              LIMIT 5
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            359661672578,75521,3.450000,3.450000,3.450000,3.450000,527.050000,23.500000,1942.890000,121.430000,408.573903
            359661748099,2254517,3.450000,3.450000,3.450000,3.450000,23.500000,23.500000,1942.890000,121.430000,408.602332
            359664002616,81,3.450000,3.450000,3.450000,3.450000,23.500000,23.500000,1942.890000,1942.890000,377.459753
            359664002697,488,3.450000,3.450000,3.450000,3.450000,527.050000,527.050000,1942.890000,1942.890000,403.624426
            359664003185,122,208.140000,3.450000,3.450000,3.450000,527.050000,527.050000,1942.890000,1942.890000,395.139180
            """))

  # Tests calculations when everything in system state is converted to mW
  def test_wattson_system_state_mw_calculations(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
              SELECT
                ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                cpu6_mw, cpu7_mw, dsu_scu_mw
              FROM _system_state_mw
              WHERE ts > 359661672577
              ORDER by ts ASC
              LIMIT 10
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            359661672578,75521,3.450000,3.450000,3.450000,3.450000,527.050000,23.500000,1942.890000,121.430000,408.573903
            359661748099,2254517,3.450000,3.450000,3.450000,3.450000,23.500000,23.500000,1942.890000,121.430000,408.602332
            359664002616,81,3.450000,3.450000,3.450000,3.450000,23.500000,23.500000,1942.890000,1942.890000,377.459753
            359664002697,488,3.450000,3.450000,3.450000,3.450000,527.050000,527.050000,1942.890000,1942.890000,403.624426
            359664003185,122,208.140000,3.450000,3.450000,3.450000,527.050000,527.050000,1942.890000,1942.890000,395.139180
            359664003307,163,208.140000,3.450000,208.140000,3.450000,527.050000,527.050000,1942.890000,1942.890000,389.643681
            359664003470,204,208.140000,3.450000,208.140000,208.140000,527.050000,527.050000,1942.890000,1942.890000,402.211569
            359664003674,11596,208.140000,208.140000,208.140000,208.140000,527.050000,527.050000,1942.890000,1942.890000,408.431816
            359664015270,4720,208.140000,208.140000,208.140000,208.140000,527.050000,527.050000,1942.890000,121.430000,408.285869
            359664019990,18921,208.140000,208.140000,208.140000,208.140000,23.500000,527.050000,1942.890000,121.430000,408.551913
            """))

  # Tests that suspend values are being skipped
  def test_wattson_suspend_calculations(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_eos_suspend.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
              SELECT
                ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                cpu6_mw, cpu7_mw, dsu_scu_mw
              FROM _system_state_mw
              WHERE ts > 24790009884888
              ORDER by ts ASC
              LIMIT 5
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            24790009907857,2784616769,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000
            24792794524626,424063,39.690000,39.690000,39.690000,39.690000,0.000000,0.000000,0.000000,0.000000,18.390000
            24792794948689,205625,39.690000,39.690000,39.690000,39.690000,0.000000,0.000000,0.000000,0.000000,18.390000
            24792795154314,19531,39.690000,39.690000,39.690000,39.690000,0.000000,0.000000,0.000000,0.000000,18.390000
            24792795173845,50781,39.690000,39.690000,0.000000,39.690000,0.000000,0.000000,0.000000,0.000000,18.390000
            """))

  # Tests that total calculations are correct
  def test_wattson_idle_attribution(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_eos_suspend.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.tasks.idle_transitions_attribution;
            SELECT
              SUM(estimated_mw * dur) / 1000000000 as idle_transition_cost_mws,
              utid,
              upid
            FROM _idle_transition_cost
            GROUP BY utid
            ORDER BY idle_transition_cost_mws DESC
            LIMIT 20
            """),
        out=Csv("""
            "idle_transition_cost_mws","utid","upid"
            19.069650,10,10
            7.642312,73,73
            6.070612,146,146
            4.887749,457,457
            4.642036,694,353
            4.576077,1262,401
            4.513604,515,137
            3.819478,169,169
            3.803996,11,11
            3.617835,147,147
            3.522914,396,396
            3.386014,486,486
            3.351159,727,356
            3.279299,606,326
            3.155206,464,464
            2.949447,29,29
            2.848224,414,414
            2.661055,471,471
            2.573043,1270,401
            2.488295,172,172
            """))

  # Tests that DSU devfreq calculations are merged correctly
  def test_wattson_dsu_devfreq_system_state(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_tk4_pcmark.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
            SELECT
               ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
               cpu6_mw, cpu7_mw, dsu_scu_mw
            FROM _system_state_mw
            WHERE ts > 4108586775197
            LIMIT 20
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            4108586789603,35685,2.670000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.145271
            4108586825288,30843,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.148554
            4108586856131,13387,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,99.470000,1270.995753
            4108586869518,22542,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.105587
            4108586892060,2482,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1270.614641
            4108586894542,68563,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.196124
            4108586963105,59652,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1271.166706
            4108587022757,3743,2.670000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1270.620321
            4108587026500,15992,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1271.070056
            4108587042492,15625,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,99.470000,1271.118234
            4108587058117,8138,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1271.005033
            4108587066255,80566,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.167766
            4108587146821,19572,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,99.470000,1271.076795
            4108587166393,219116,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.200356
            4108587385509,81991,205.600000,2.670000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.193880
            4108587467500,90413,205.600000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.186713
            4108587557913,92896,2.670000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.180805
            4108587650809,95296,205.600000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.190927
            4108587746105,12451,2.670000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.006475
            4108587758556,28524,2.670000,2.670000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1271.130924
            """))

  def test_wattson_time_window_api(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query="""
        INCLUDE PERFETTO MODULE wattson.estimates;

        SELECT
          cpu0_mw,
          cpu1_mw,
          cpu2_mw,
          cpu3_mw,
          cpu4_mw,
          cpu5_mw,
          cpu6_mw,
          cpu7_mw,
          dsu_scu_mw
        FROM _wattson_base_components_avg_mw!(
          (SELECT 362426061658 AS ts, 5067704349 AS dur, 0 AS period_id)
        )
        """,
        out=Csv("""
            "cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            13.123951,6.319792,5.482452,8.870058,8.940995,10.722844,29.494919,30.193860,25.745894
        """))

  # Tests that suspend calculations are correct on 8 CPU device where suspend
  # indication comes from "syscore" command
  def test_wattson_syscore_suspend(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_syscore_suspend.pb'),
        query=("""
            INCLUDE PERFETTO MODULE intervals.intersect;
            INCLUDE PERFETTO MODULE wattson.estimates;

            SELECT
              ii.ts,
              ii.dur,
              ss.power_state = 'suspended' AS suspended
            FROM _interval_intersect!(
              (
                _ii_subquery!(_w_cpu_slices_raw),
                _ii_subquery!(android_suspend_state)
              ),
              ()
            ) AS ii
            JOIN _w_cpu_slices_raw AS stats
              ON stats._auto_id = id_0
            JOIN android_suspend_state AS ss
              ON ss._auto_id = id_1
            WHERE suspended
            """),
        out=Csv("""
            "ts","dur","suspended"
            385019771468,61975407053,1
            448320364476,3674872885,1
            452415394221,69579176303,1
            564873995228,135118729231,1
            """))

  # Tests traces from VM that have incomplete CPU tracks
  def test_wattson_missing_cpus_on_guest(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_tk4_vm.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
               SELECT
                 ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                 cpu6_mw
               FROM _system_state_mw
               WHERE ts > 25150000000
               LIMIT 10
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw"
            25150029000,1080,0.000000,0.000000,0.000000,0.000000,70.050000,83.260000,0.000000
            25150030080,560,0.000000,0.000000,0.000000,0.000000,70.050000,70.050000,0.000000
            25150030640,42920,0.000000,0.000000,0.000000,0.000000,70.050000,70.050000,0.000000
            25150073560,99800,0.000000,0.000000,0.000000,0.000000,70.050000,0.000000,0.000000
            25150173360,28240,176.280000,0.000000,0.000000,0.000000,70.050000,0.000000,0.000000
            25150201600,6480,176.280000,0.000000,0.000000,176.280000,70.050000,0.000000,0.000000
            25150208080,29840,176.280000,0.000000,0.000000,176.280000,70.050000,70.050000,0.000000
            25150237920,129800,0.000000,0.000000,0.000000,176.280000,70.050000,70.050000,0.000000
            25150367720,37480,0.000000,0.000000,0.000000,176.280000,70.050000,0.000000,0.000000
            25150405200,15120,0.000000,176.280000,0.000000,176.280000,70.050000,0.000000,0.000000
            """))

  # Tests suspend path with devfreq code path
  def test_wattson_devfreq_hotplug_and_suspend(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_cpuhp_devfreq_suspend.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
               SELECT
                 ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                 cpu6_mw, cpu7_mw, dsu_scu_mw
               FROM _system_state_mw
               WHERE ts > 165725449108
              LIMIT 6
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            165725450194,7527,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,375.490000,63.790000
            165725457721,17334,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,63.790000
            165725475055,6999,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,49.230000
            165725482054,1546,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,63.790000
            165725483600,4468465,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,63.790000
            165729952065,73480460119,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000
            """))

  # Tests trace with both 1D and 2D static calculations
  def test_wattson_multi_static_calc(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_sxr_multi_static.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
               SELECT
                 ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
                 cpu6_mw, cpu7_mw, dsu_scu_mw
               FROM _system_state_mw
               ORDER BY ts ASC
               LIMIT 10
            """),
        out=Csv("""
            "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            70591689312236,573,17.290000,"[NULL]",22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,58.220000
            70591689312809,28281,17.290000,682.860000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,58.220000
            70591689341090,13333,15.060000,647.090000,22.690000,22.690000,"[NULL]",22.690000,0.000000,0.000000,60.920000
            70591689354423,7031,17.290000,682.860000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,58.220000
            70591689361454,573,10.130000,682.860000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,44.220000
            70591689362027,13229,10.130000,446.460000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,44.220000
            70591689375256,14636,10.130000,10.130000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,44.220000
            70591689389892,417,10.130000,10.130000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,44.220000
            70591689390309,10208,10.130000,10.130000,22.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,44.220000
            70591689400517,11458,10.130000,10.130000,529.690000,529.690000,"[NULL]",22.690000,0.000000,0.000000,44.220000
            """))

  # Tests remapping of idle states
  def test_wattson_idle_remap(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_idle_map.pb'),
        query=("""
               INCLUDE PERFETTO MODULE wattson.estimates;
               SELECT ts, dur, cpu, idle
               FROM _adjusted_deep_idle
               WHERE ts > 1450338950433 AND cpu = 3
               LIMIT 10
               """),
        out=Csv("""
               "ts","dur","cpu","idle"
               1450338950434,1395365,3,1
               1450340345799,96927,3,-1
               1450340442726,301250,3,0
               1450340743976,24010,3,-1
               1450340767986,3748386,3,1
               1450344516372,70208,3,-1
               1450344586580,2400521,3,1
               1450346987101,306458,3,-1
               1450347293559,715573,3,0
               1450348009132,82292,3,-1
               """))

  # Tests that hotplug slices that defined CPU off region are correct
  def test_wattson_hotplug_tk(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_cpuhp_devfreq_suspend.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.cpu.hotplug;
            SELECT cpu, ts, dur
            FROM _gapless_hotplug_slices
            WHERE cpu < 2
            """),
        out=Csv("""
            "cpu","ts","dur"
            0,86747008512,302795933205
            1,86747008512,3769632400
            1,90516640912,4341919
            1,90520982831,73692291133
            1,164213273964,1478796428
            1,165692070392,73525895666
            1,239217966058,10896074956
            1,250114041014,95948
            1,250114136962,4705159
            1,250118842121,137102890041
            1,387221732162,2321209555
            """))

  # Tests that IRQ stacks are properly flattened and have unique IDs
  def test_wattson_irq_flattening(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_irq_gpu_markers.pb'),
        query="""
        INCLUDE PERFETTO MODULE wattson.tasks.task_slices;

        SELECT
          SUM(dur) AS total_dur, irq_name, irq_id
        FROM  _all_irqs_flattened_slices
        GROUP BY irq_name
        LIMIT 10
        """,
        out=Csv("""
          "total_dur","irq_name","irq_id"
          1118451,"BLOCK",-7563548160659491326
          1701414,"IRQ (100a0000.BIG)",-8960469306195608742
          769330,"IRQ (100a0000.LITTLE)",2595235052520049942
          741289,"IRQ (100a0000.MID)",709594339438163430
          2179935,"IRQ (10840000.pinctrl)",6369664009351169759
          1192993,"IRQ (10970000.hsi2c)",-1238860297262945668
          7840694,"IRQ (176a0000.mbox)",442503679933451729
          2110993,"IRQ (1c0b0000.drmdpp)",3108582083943637163
          2132254,"IRQ (1c0b1000.drmdpp)",2330704911466106250
          1187454,"IRQ (1c0b2000.drmdpp)",-4397375750993244671
          """))

  # Tests that all tasks are correct after accounting for preemption and idle
  # exits
  def test_wattson_all_tasks_flattening_and_idle_exits(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_irq_gpu_markers.pb'),
        query="""
        INCLUDE PERFETTO MODULE wattson.tasks.task_slices;

        SELECT
          SUM(dur) AS dur,
          thread_name
        FROM _wattson_task_slices
        JOIN _wattson_task_metadata USING (utid)
        GROUP BY thread_name
        ORDER BY dur DESC
        LIMIT 10
        """,
        out=Csv("""
          "dur","thread_name"
          80559339989,"swapper"
          1617087785,"Runner: gl_tess"
          800487950,"mali-cmar-backe"
          469271586,"mali_jd_thread"
          426019439,"surfaceflinger"
          326858956,"IRQ (exynos-mct)"
          323531361,"s.nexuslauncher"
          312153973,"RenderThread"
          251889143,"50000.corporate"
          241043219,"traced_probes"
          """))

  # Tests freq dependent static along with DSU dependent static calculations on
  # the same device
  def test_wattson_multi_variant_static(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_freq_dep_static.pb'),
        query="""
          INCLUDE PERFETTO MODULE wattson.estimates;
             SELECT
               ts, dur, cpu0_mw, cpu1_mw, cpu2_mw, cpu3_mw, cpu4_mw, cpu5_mw,
               cpu6_mw, cpu7_mw, dsu_scu_mw
             FROM _system_state_mw
             WHERE ts >= 11209755572327
             LIMIT 5
        """,
        out=Csv("""
          "ts","dur","cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
          11209755572327,32239,100.800000,100.800000,3.360000,30.950000,30.950000,473.600000,473.600000,2244.540000,149.839659
          11209755604566,43021,100.800000,100.800000,30.950000,30.950000,30.950000,473.600000,473.600000,2244.540000,149.837743
          11209755647587,3776,139.420000,100.800000,30.950000,30.950000,30.950000,473.600000,473.600000,2244.540000,162.226139
          11209755651363,50651,139.420000,139.420000,30.950000,30.950000,30.950000,473.600000,473.600000,2244.540000,162.249919
          11209755702014,8177,139.420000,139.420000,30.950000,3.360000,30.950000,473.600000,473.600000,2244.540000,162.224294
          """))

  def test_wattson_tpu_estimate(self):
    return DiffTestBlueprint(
        trace=DataPath('tpu_jumpnet.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
            SELECT
              SUM(tpu_mw * dur) / SUM(dur) as avg_tpu_mw,
              SUM(tpu_mw * dur) / 1e9 as total_tpu_mws
            FROM _system_state_mw
            WHERE tpu_mw IS NOT NULL
            """),
        out=Csv("""
            "avg_tpu_mw","total_tpu_mws"
            43.459353,592.702934
            """))

  # Verify intermediate GPU active region extraction
  def test_wattson_gpu_active_regions(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_gpu_attribution.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.tasks.gpu_active_regions;
            SELECT ts, dur FROM _gpu_active_regions ORDER BY ts ASC LIMIT 5;
            """),
        out=Csv("""
            "ts","dur"
            13399714638761,10769428
            13399727319204,5759219
            13399741522303,7668750
            13399760952069,6746745
            13399768883996,4985000
            """))

  # Verify final standalone GPU attribution output
  def test_wattson_gpu_attribution(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_gpu_attribution.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.tasks.attribution;
            SELECT ts, dur, package_name, estimated_mw, idle_mw
            FROM _gpu_estimates_w_tasks_attribution
            WHERE estimated_mw > 0 OR idle_mw > 0
            ORDER BY ts ASC LIMIT 5;
            """),
        out=Csv("""
            "ts","dur","package_name","estimated_mw","idle_mw"
            13399714825233,44479,"android",750.380000,0.000000
            13399714869712,28333,"GPU Idle",0.000000,750.380000
            13399714898045,48464,"android",750.380000,0.000000
            13399714946509,263607,"GPU Idle",0.000000,750.380000
            13399715210116,1828255,"GPU Idle",0.000000,750.380000
            """))

  def test_wattson_multiple_1d_static(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_multiple_1d_static.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
            SELECT ts, dur, static_1d
            FROM _w_independent_cpus_calc
            WHERE ts >= 159400423427
            LIMIT 15
            """),
        out=Csv("""
            "ts","dur","static_1d"
            159400423427,47188,20.790000
            159400470615,51875,45.340000
            159400522490,625,83.940000
            159400523115,573,83.940000
            159400523688,469,83.940000
            159400524157,520,83.940000
            159400524677,469,83.940000
            159400525146,6042,83.940000
            159400531188,40052,83.940000
            159400571240,49375,83.940000
            159400620615,250781,59.390000
            159400871396,59844,59.390000
            159400931240,142656,59.390000
            159401073896,17031,59.390000
            159401090927,37032,59.390000
            """))

  def test_wattson_ui_continuous_estimates(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.ui.continuous_estimates;
            SELECT * FROM (
              SELECT 'cpu0' AS track, ts, dur, round(cpu0_mw, 4) AS val, cpu
              FROM _system_state_cpu0_mw WHERE cpu0_mw > 0 ORDER BY dur DESC LIMIT 2
            )
            UNION ALL
            SELECT * FROM (
              SELECT 'cpu7' AS track, ts, dur, round(cpu7_mw, 4) AS val, cpu
              FROM _system_state_cpu7_mw WHERE cpu7_mw > 0 ORDER BY dur DESC LIMIT 2
            )
            UNION ALL
            SELECT * FROM (
              SELECT 'dsu' AS track, ts, dur, round(dsu_scu_mw, 4) AS val, -1 AS cpu
              FROM _system_state_dsu_scu_mw WHERE dsu_scu_mw > 0 ORDER BY dur DESC LIMIT 2
            );
            """),
        out=Csv("""
            "track","ts","dur","val","cpu"
            "cpu0",367581909643,2521866334,1.090000,0
            "cpu0",359764107271,4207031,269.910000,0
            "cpu7",367581893611,2521882366,306.570000,7
            "cpu7",360252060721,43523194,1942.890000,7
            "dsu",367581909643,2521866334,69.819200,-1
            "dsu",361952679578,10225138,0.489700,-1
            """))

  def test_wattson_dsu_devfreq_gapless(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.12.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "google,malibu-grizzly"
            device_tree_compatibles: "google,malibu"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 1
              cpu_frequency {
                state: 394000
                cpu_id: 0
              }
            }
            event {
              timestamp: 3000
              pid: 1
              devfreq_frequency {
                dev_name: "dsu_target"
                freq: 768000000
              }
            }
            event {
              timestamp: 5000
              pid: 1
              cpu_frequency {
                state: 533000
                cpu_id: 0
              }
            }
          }
        }
        """),
        query=("""
            INCLUDE PERFETTO MODULE wattson.cpu.arm_dsu;
            SELECT ts, dur, dsu_freq FROM _wattson_dsu_frequency ORDER BY ts
            """),
        out=Csv("""
            "ts","dur","dsu_freq"
            1000,2000,394000000
            3000,2000,768000000
            """))

  def test_wattson_soc_model_ingestion(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.wattson_soc_model] {
            device_name: "Tensor G6"
            gpu_id: 0
            use_devfreq: true
            model_version: 1
            model_source: MODEL_SOURCE_VENDOR
            cpu_clusters {
              cluster: 0
              cpus: [0, 1, 2, 3]
              deep_idle_offset_ns: 0
            }
            cpu_clusters {
              cluster: 4
              cpus: [4, 5, 6]
            }
            cpu_clusters {
              cluster: 7
              cpus: [7]
              deep_idle_offset_ns: 200000
              vote_by_freq: true
            }
            idle_state_map {
              nominal_idle: 2
              override_idle: 1
            }
            subsystems {
              name: "cpu"
              curves {
                cluster: 0
                freq: 1000000
                static_mw: 5.5
                active_mw: [45.0]
                idle_mw: [1.25, 0.125]
              }
              curves {
                cluster: 4
                freq: 2000000
                static_mw: 9.5
                active_mw: [120.0]
                idle_mw: [3.75]
              }
              curves {
                cluster: 255
                freq: 394000000
                active_mw: [56.5]
              }
              curves {
                cluster: 7
                freq: 3000000
                dep_cluster: 4
                dep_freq: 2000000
                static_mw: 25.0
                active_mw: [350.0]
                idle_mw: [2.5, 0.25]
                interconnect_mw: 18.0
                l3_hit: 12.5
                l3_miss: 34.0
              }
            }
            subsystems {
              name: "gpu"
              curves {
                freq: 800000
                active_mw: [280.0]
                idle_mw: [6.0, 1.5]
              }
            }
            subsystems {
              name: "tpu"
              curves {
                cluster: 0
                freq: 1200000
                active_mw: [210.0, 420.0]
              }
            }
          }
        }
        """),
        query="""
        SELECT
          'device_info' AS tbl,
          printf(
            '%s|devfreq=%d|gpu=%d|v=%d|src=%d',
            device_name, use_devfreq, gpu_id, model_version, model_source
          ) AS row_data
        FROM __intrinsic_wattson_external_device_info
        UNION ALL
        SELECT
          'cpu_cluster',
          printf(
            'cpu=%d|cluster=%d|offset=%d|vote_freq=%d',
            cpu, cluster, deep_idle_offset_ns, vote_by_freq
          )
        FROM __intrinsic_wattson_external_cpu_cluster
        UNION ALL
        SELECT
          'idle_map',
          printf('nom=%d|ovr=%d', nominal_idle, override_idle)
        FROM __intrinsic_wattson_external_idle_state_map
        UNION ALL
        SELECT
          'cpu_1d',
          printf(
            'c=%d|f=%d|s=%.2f|a=%.2f|i0=%.3f|i1=%.3f',
            cluster, freq_khz, static_mw, active, idle0, idle1
          )
        FROM __intrinsic_wattson_external_curves_cpu_1d
        UNION ALL
        SELECT
          'cpu_2d',
          printf(
            'c=%d|f=%d|dc=%d|df=%d|s=%.1f|a=%.1f|i0=%.2f|i1=%.2f|ic=%.1f',
            cluster, freq_khz, dep_cluster, dep_freq,
            static_mw, active, idle0, idle1, interconnect
          )
        FROM __intrinsic_wattson_external_curves_cpu_2d
        UNION ALL
        SELECT
          'l3',
          printf(
            'f=%d|dc=%d|df=%d|hit=%.1f|miss=%.1f',
            freq_khz, dep_cluster, dep_freq, l3_hit, l3_miss
          )
        FROM __intrinsic_wattson_external_curves_l3
        UNION ALL
        SELECT
          'gpu',
          printf('f=%d|a=%.1f|i1=%.1f|i2=%.1f', freq_khz, active, idle1, idle2)
        FROM __intrinsic_wattson_external_curves_gpu
        UNION ALL
        SELECT
          'tpu',
          printf('c=%d|req=%d|f=%d|a=%.1f', cluster, requests, freq, active)
        FROM __intrinsic_wattson_external_curves_tpu;
        """,
        out=Csv("""
        "tbl","row_data"
        "device_info","Tensor G6|devfreq=1|gpu=0|v=1|src=1"
        "cpu_cluster","cpu=0|cluster=0|offset=0|vote_freq=0"
        "cpu_cluster","cpu=1|cluster=0|offset=0|vote_freq=0"
        "cpu_cluster","cpu=2|cluster=0|offset=0|vote_freq=0"
        "cpu_cluster","cpu=3|cluster=0|offset=0|vote_freq=0"
        "cpu_cluster","cpu=4|cluster=4|offset=0|vote_freq=0"
        "cpu_cluster","cpu=5|cluster=4|offset=0|vote_freq=0"
        "cpu_cluster","cpu=6|cluster=4|offset=0|vote_freq=0"
        "cpu_cluster","cpu=7|cluster=7|offset=200000|vote_freq=1"
        "idle_map","nom=2|ovr=1"
        "cpu_1d","c=0|f=1000000|s=5.50|a=45.00|i0=1.250|i1=0.125"
        "cpu_1d","c=4|f=2000000|s=9.50|a=120.00|i0=3.750|i1=0.000"
        "cpu_1d","c=255|f=394000000|s=0.00|a=56.50|i0=0.000|i1=0.000"
        "cpu_2d","c=7|f=3000000|dc=4|df=2000000|s=25.0|a=350.0|i0=2.50|i1=0.25|ic=18.0"
        "l3","f=3000000|dc=4|df=2000000|hit=12.5|miss=34.0"
        "gpu","f=800000|a=280.0|i1=6.0|i2=1.5"
        "tpu","c=0|req=0|f=1200000|a=210.0"
        "tpu","c=0|req=1|f=1200000|a=420.0"
        """))

  def test_wattson_soc_model_duplicate_ignored(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.wattson_soc_model] {
            device_name: "Tensor G6"
            subsystems {
              name: "cpu"
              curves {
                cluster: 0
                freq: 1000000
                active_mw: [45.0]
                idle_mw: [1.25]
              }
            }
          }
        }
        packet {
          timestamp: 2000
          [com.android.internal.FrameworksBaseTracePacket.wattson_soc_model] {
            device_name: "Tensor G5"
            subsystems {
              name: "cpu"
              curves {
                cluster: 0
                freq: 2000000
                active_mw: [90.0]
                idle_mw: [2.5]
              }
            }
          }
        }
        """),
        query="""
        SELECT d.device_name, c.cluster, c.freq_khz, c.active, c.idle0
        FROM __intrinsic_wattson_external_device_info AS d
        CROSS JOIN __intrinsic_wattson_external_curves_cpu_1d AS c;
        """,
        out=Csv("""
        "device_name","cluster","freq_khz","active","idle0"
        "Tensor G6",0,1000000,45.000000,1.250000
        """))

  def test_wattson_soc_model_malformed_curves_ignored(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.wattson_soc_model] {
            device_name: "Tensor G6"
            subsystems {
              name: "cpu"
              # 1. Regular CPU 1D curve missing idle_mw -> skipped.
              curves { cluster: 0 freq: 1000000 active_mw: [45.0] }
              # 2. Regular CPU 1D curve missing active_mw -> skipped.
              curves { cluster: 0 freq: 1100000 idle_mw: [1.25] }
              # 3. Unpaired dep_cluster without dep_freq -> skipped.
              curves {
                cluster: 7 freq: 3000000 dep_cluster: 4
                active_mw: [350.0] idle_mw: [2.5]
              }
              # 4. Unpaired dep_freq without dep_cluster -> skipped.
              curves {
                cluster: 7 freq: 3100000 dep_freq: 2000000
                active_mw: [350.0] idle_mw: [2.5]
              }
              # 5. Pure L3 2D curve -> populates L3 table only, not Cpu2D.
              curves {
                cluster: 0 freq: 820000 dep_cluster: 255 dep_freq: 610000
                l3_hit: 0.5 l3_miss: 0.875
              }
              # 6. Unpaired l3_hit without l3_miss -> skipped.
              curves {
                cluster: 0 freq: 955000 dep_cluster: 255 dep_freq: 610000
                l3_hit: 0.5
              }
            }
            subsystems {
              name: "gpu"
              # 7. GPU curve missing idle_mw -> skipped.
              curves { freq: 800000 active_mw: [280.0] }
            }
            subsystems {
              name: "tpu"
              # 8. TPU curve missing cluster -> skipped.
              curves { freq: 1200000 active_mw: [210.0] }
            }
          }
        }
        """),
        query="""
        SELECT
          (SELECT count(*) FROM __intrinsic_wattson_external_curves_cpu_1d) AS c1d_cnt,
          (SELECT count(*) FROM __intrinsic_wattson_external_curves_cpu_2d) AS c2d_cnt,
          (SELECT count(*) FROM __intrinsic_wattson_external_curves_gpu) AS gpu_cnt,
          (SELECT count(*) FROM __intrinsic_wattson_external_curves_tpu) AS tpu_cnt,
          freq_khz,
          dep_cluster,
          dep_freq,
          l3_hit,
          l3_miss
        FROM __intrinsic_wattson_external_curves_l3;
        """,
        out=Csv("""
        "c1d_cnt","c2d_cnt","gpu_cnt","tpu_cnt","freq_khz","dep_cluster","dep_freq","l3_hit","l3_miss"
        0,0,0,0,820000,255,610000,0.500000,0.875000
        """))

  def test_wattson_soc_model_overrides_intree_stdlib(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.1.0"
              machine: "aarch64"
            }
            device_tree_compatibles: "google,gs101"
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 1000
          [com.android.internal.FrameworksBaseTracePacket.wattson_soc_model] {
            device_name: "Custom_OOT_SoC"
            gpu_id: 2
            use_devfreq: true
            cpu_clusters {
              cluster: 0
              cpus: [0, 1]
              deep_idle_offset_ns: 123000
              vote_by_freq: true
            }
            idle_state_map {
              nominal_idle: 2
              override_idle: 1
            }
            subsystems {
              name: "cpu"
              curves {
                cluster: 0 freq: 1000000 static_mw: 4.0
                active_mw: [40.0] idle_mw: [1.0, 0.2]
              }
              curves {
                cluster: 255 freq: 500000000 active_mw: [55.0]
              }
              curves {
                cluster: 0 freq: 2000000 dep_cluster: 255 dep_freq: 500000000
                static_mw: 8.0 active_mw: [110.0] idle_mw: [2.0, 0.5]
                interconnect_mw: 12.0 l3_hit: 9.0 l3_miss: 27.0
              }
            }
            subsystems {
              name: "gpu"
              curves { freq: 750000 active_mw: [250.0] idle_mw: [5.0, 1.0] }
            }
            subsystems {
              name: "tpu"
              curves { cluster: 0 freq: 1100000 active_mw: [180.0, 360.0] }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE wattson.curves.utils;
        SELECT 'device' AS tbl, name AS val FROM _wattson_device
        UNION ALL
        SELECT 'policy_map', printf('cpu=%d|pol=%d', cpu, policy)
        FROM _dev_cpu_policy_map
        UNION ALL
        SELECT 'vote_freq', printf('cpu=%d|vf=%d', cpu, vote_by_freq)
        FROM _dev_vote_by_freq
        UNION ALL
        SELECT 'idle_ovr', printf('nom=%d|ovr=%d', nominal_idle, override_idle)
        FROM _idle_state_map_override
        UNION ALL
        SELECT 'devfreq', printf('%d', devfreq_necessary)
        FROM _use_devfreq_for_calc
        UNION ALL
        SELECT 'gpu_id', printf('%s|%d', device, gpu_id) FROM _gpuid_map
        UNION ALL
        SELECT 'c1d', printf('p=%d|f=%d|a=%.1f', policy, freq_khz, active)
        FROM _filtered_curves_1d_raw
        UNION ALL
        SELECT 'dsu_1d', printf('f=%d|p=%.1f', dsu_freq, power)
        FROM _filtered_curves_dsu_1d
        UNION ALL
        SELECT
          'c2d',
          printf(
            'p=%d|f=%d|dp=%d|df=%d|a=%.1f|ic=%.1f',
            policy, freq_khz, dep_policy, dep_freq, active, interconnect
          )
        FROM _filtered_curves_2d_raw
        UNION ALL
        SELECT
          'l3',
          printf(
            'f=%d|dp=%d|df=%d|hit=%.1f|miss=%.1f',
            freq_khz, dep_policy, dep_freq, l3_hit, l3_miss
          )
        FROM _filtered_curves_l3
        UNION ALL
        SELECT 'gpu', printf('f=%d|a=%.1f|i1=%.1f|i2=%.1f', freq_khz, active, idle1, idle2)
        FROM _gpu_filtered_curves_raw
        UNION ALL
        SELECT 'tpu', printf('c=%d|req=%d|f=%d|a=%.1f', cluster, requests, freq, active)
        FROM _tpu_filtered_curves;
        """,
        out=Csv("""
        "tbl","val"
        "device","Custom_OOT_SoC"
        "policy_map","cpu=0|pol=0"
        "policy_map","cpu=1|pol=0"
        "vote_freq","cpu=0|vf=1"
        "vote_freq","cpu=1|vf=1"
        "idle_ovr","nom=2|ovr=1"
        "devfreq","1"
        "gpu_id","Custom_OOT_SoC|2"
        "c1d","p=0|f=1000000|a=40.0"
        "c1d","p=0|f=1000000|a=40.0"
        "dsu_1d","f=500000000|p=55.0"
        "c2d","p=0|f=2000000|dp=255|df=500000000|a=110.0|ic=12.0"
        "l3","f=2000000|dp=255|df=500000000|hit=9.0|miss=27.0"
        "gpu","f=750000|a=250.0|i1=5.0|i2=1.0"
        "tpu","c=0|req=0|f=1100000|a=180.0"
        "tpu","c=0|req=1|f=1100000|a=360.0"
        """))
