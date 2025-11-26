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

from python.generators.diff_tests.testing import Csv, Path, DataPath
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

  # Tests intermediate table
  def test_wattson_intermediate_table(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_dsu_pmu.pb'),
        query=("""
            INCLUDE PERFETTO MODULE wattson.estimates;
              select
                ts,dur,l3_hit_count,l3_miss_count,freq_0,idle_0,freq_1,idle_1,freq_2,idle_2,freq_3,idle_3,freq_4,idle_4,freq_5,idle_5,freq_6,idle_6,freq_7,idle_7,no_static,cpu4_curve,cpu5_curve,cpu6_curve,cpu7_curve
              from _w_independent_cpus_calc
              WHERE ts > 359661672577
              ORDER by ts ASC
              LIMIT 10
            """),
        out=Csv("""
          "ts","dur","l3_hit_count","l3_miss_count","freq_0","idle_0","freq_1","idle_1","freq_2","idle_2","freq_3","idle_3","freq_4","idle_4","freq_5","idle_5","freq_6","idle_6","freq_7","idle_7","no_static","cpu4_curve","cpu5_curve","cpu6_curve","cpu7_curve"
          359661672578,75521,8326,9689,1401000,0,1401000,0,1401000,0,1401000,0,2253000,-1,2253000,0,2802000,-1,2802000,0,0,527.050000,23.500000,1942.890000,121.430000
          359661748099,2254517,248577,289258,1401000,0,1401000,0,1401000,0,1401000,0,2253000,0,2253000,0,2802000,-1,2802000,0,0,23.500000,23.500000,1942.890000,121.430000
          359664002616,81,8,10,1401000,0,1401000,0,1401000,0,1401000,0,2253000,0,2253000,0,2802000,-1,2802000,-1,0,23.500000,23.500000,1942.890000,1942.890000
          359664002697,488,53,62,1401000,0,1401000,0,1401000,0,1401000,0,2253000,-1,2253000,-1,2802000,-1,2802000,-1,0,527.050000,527.050000,1942.890000,1942.890000
          359664003185,122,13,15,1401000,-1,1401000,0,1401000,0,1401000,0,2253000,-1,2253000,-1,2802000,-1,2802000,-1,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003307,163,17,20,1401000,-1,1401000,0,1401000,-1,1401000,0,2253000,-1,2253000,-1,2802000,-1,2802000,-1,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003470,204,22,26,1401000,-1,1401000,0,1401000,-1,1401000,-1,2253000,-1,2253000,-1,2802000,-1,2802000,-1,-1,527.050000,527.050000,1942.890000,1942.890000
          359664003674,11596,1278,1487,1401000,-1,1401000,-1,1401000,-1,1401000,-1,2253000,-1,2253000,-1,2802000,-1,2802000,-1,-1,527.050000,527.050000,1942.890000,1942.890000
          359664015270,4720,520,605,1401000,-1,1401000,-1,1401000,-1,1401000,-1,2253000,-1,2253000,-1,2802000,-1,2802000,0,-1,527.050000,527.050000,1942.890000,121.430000
          359664019990,18921,2086,2427,1401000,-1,1401000,-1,1401000,-1,1401000,-1,2253000,0,2253000,-1,2802000,-1,2802000,0,-1,23.500000,527.050000,1942.890000,121.430000
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
            4108586789603,35685,2.670000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.695271
            4108586825288,30843,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.698554
            4108586856131,13387,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,99.470000,1166.545753
            4108586869518,22542,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.655587
            4108586892060,2482,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1166.164641
            4108586894542,68563,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.746124
            4108586963105,59652,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1166.716706
            4108587022757,3743,2.670000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1166.170321
            4108587026500,15992,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1166.620056
            4108587042492,15625,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,99.470000,1166.668234
            4108587058117,8138,205.600000,205.600000,205.600000,2.670000,674.240000,674.240000,674.240000,3327.560000,1166.555033
            4108587066255,80566,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.717766
            4108587146821,19572,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,99.470000,1166.626795
            4108587166393,219116,205.600000,205.600000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.750356
            4108587385509,81991,205.600000,2.670000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.743880
            4108587467500,90413,205.600000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.736713
            4108587557913,92896,2.670000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.730805
            4108587650809,95296,205.600000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.740927
            4108587746105,12451,2.670000,2.670000,2.670000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.556475
            4108587758556,28524,2.670000,2.670000,205.600000,205.600000,674.240000,674.240000,674.240000,3327.560000,1166.680924
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
        FROM _windowed_system_state_mw(362426061658, 5067704349)
        """,
        out=Csv("""
            "cpu0_mw","cpu1_mw","cpu2_mw","cpu3_mw","cpu4_mw","cpu5_mw","cpu6_mw","cpu7_mw","dsu_scu_mw"
            13.119297,6.317755,5.480736,8.867040,8.940129,10.721293,29.491222,30.247726,25.756884
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
              stats.cpu0_id,
              stats.cpu1_id,
              stats.cpu2_id,
              stats.cpu3_id,
              ss.power_state = 'suspended' AS suspended
            FROM _interval_intersect!(
              (
                _ii_subquery!(_stats_cpu0123),
                _ii_subquery!(android_suspend_state)
              ),
              ()
            ) AS ii
            JOIN _stats_cpu0123 AS stats
              ON stats._auto_id = id_0
            JOIN android_suspend_state AS ss
              ON ss._auto_id = id_1
            WHERE suspended
            """),
        out=Csv("""
            "ts","dur","cpu0_id","cpu1_id","cpu2_id","cpu3_id","suspended"
            385019771468,61975407053,12042,12219,10489,8911,1
            448320364476,3674872885,13008,12957,11169,9275,1
            452415394221,69579176303,13659,13366,11656,9614,1
            564873995228,135118729231,45230,37601,22805,20139,1
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
            165725450194,7527,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,375.490000,14.560000
            165725457721,17334,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,14.560000
            165725475055,6999,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000
            165725482054,1546,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,14.560000
            165725483600,4468465,111.020000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,14.560000
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
          11209755572327,32239,100.800000,100.800000,3.360000,30.950000,30.950000,100.980000,100.980000,1959.070000,144.309659
          11209755604566,43021,100.800000,100.800000,30.950000,30.950000,30.950000,100.980000,100.980000,1959.070000,144.307743
          11209755647587,3776,139.420000,100.800000,30.950000,30.950000,30.950000,100.980000,100.980000,1959.070000,156.206139
          11209755651363,50651,139.420000,139.420000,30.950000,30.950000,30.950000,100.980000,100.980000,1959.070000,156.229919
          11209755702014,8177,139.420000,139.420000,30.950000,3.360000,30.950000,100.980000,100.980000,1959.070000,156.204294
          """))
