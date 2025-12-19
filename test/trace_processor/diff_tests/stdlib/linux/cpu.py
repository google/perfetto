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

from python.generators.diff_tests.testing import Csv, DataPath, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class LinuxCpu(TestSuite):

  def test_cpu_utilization_per_second(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE linux.cpu.utilization.system;

        SELECT * FROM cpu_utilization_per_second;
        """,
        out=Csv("""
        "ts","utilization","unnormalized_utilization"
        70000000000,0.004545,0.036362
        71000000000,0.022596,0.180764
        72000000000,0.163393,1.307146
        73000000000,0.452122,3.616972
        74000000000,0.525557,4.204453
        75000000000,0.388632,3.109057
        76000000000,0.425447,3.403579
        77000000000,0.201112,1.608896
        78000000000,0.280247,2.241977
        79000000000,0.345228,2.761827
        80000000000,0.303258,2.426064
        81000000000,0.487522,3.900172
        82000000000,0.080542,0.644336
        83000000000,0.362450,2.899601
        84000000000,0.076438,0.611501
        85000000000,0.110689,0.885514
        86000000000,0.681488,5.451901
        87000000000,0.808331,6.466652
        88000000000,0.941768,7.534142
        89000000000,0.480556,3.844446
        90000000000,0.453268,3.626142
        91000000000,0.280310,2.242478
        92000000000,0.006381,0.051049
        93000000000,0.030991,0.247932
        94000000000,0.031981,0.255845
        95000000000,0.027931,0.223446
        96000000000,0.063066,0.504529
        97000000000,0.023847,0.190773
        98000000000,0.011291,0.090328
        99000000000,0.024065,0.192518
        100000000000,0.001964,0.015711
        """))

  def test_cpu_process_utilization_per_second(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE linux.cpu.utilization.process;

        SELECT *
        FROM cpu_process_utilization_per_second(10);
        """,
        out=Csv("""
        "ts","utilization","unnormalized_utilization"
        72000000000,0.000187,0.001495
        73000000000,0.000182,0.001460
        77000000000,0.000072,0.000579
        78000000000,0.000275,0.002204
        82000000000,0.000300,0.002404
        83000000000,0.000004,0.000034
        87000000000,0.000133,0.001065
        88000000000,0.000052,0.000416
        89000000000,0.000212,0.001697
        92000000000,0.000207,0.001658
        97000000000,0.000353,0.002823
        """))

  def test_cpu_thread_utilization_per_second(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE linux.cpu.utilization.thread;

        SELECT *
        FROM cpu_thread_utilization_per_second(10);
        """,
        out=Csv("""
        "ts","utilization","unnormalized_utilization"
        70000000000,0.000024,0.000195
        72000000000,0.000025,0.000200
        73000000000,0.000053,0.000420
        74000000000,0.000044,0.000352
        75000000000,0.000058,0.000461
        76000000000,0.000075,0.000603
        77000000000,0.000051,0.000407
        78000000000,0.000047,0.000374
        79000000000,0.000049,0.000396
        80000000000,0.000084,0.000673
        81000000000,0.000041,0.000329
        82000000000,0.000048,0.000383
        83000000000,0.000040,0.000323
        84000000000,0.000018,0.000145
        85000000000,0.000053,0.000421
        86000000000,0.000121,0.000972
        87000000000,0.000049,0.000392
        88000000000,0.000036,0.000285
        89000000000,0.000033,0.000266
        90000000000,0.000050,0.000401
        91000000000,0.000025,0.000201
        92000000000,0.000009,0.000071
        """))

  def test_cpu_cycles(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.system;

             SELECT
              millicycles,
              megacycles,
              runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles;
             """),
        out=Csv("""
        "millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
        36093928491870,36093,17131594098,500000,2850000,2106863
            """))

  def test_cpu_cycles_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.system;

             SELECT
              millicycles,
              megacycles,
              runtime,
              awake_runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_in_interval(TRACE_START(), TRACE_DUR() / 10);
             """),
        out=Csv("""
          "millicycles","megacycles","runtime","awake_runtime","min_freq","max_freq","avg_freq"
          31636287288,31,76193077,76193077,614400,1708800,660492
            """))

  def test_cpu_utilization_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.system;

             SELECT
              awake_dur,
              awake_utilization,
              awake_unnormalized_utilization
             FROM cpu_utilization_in_interval(TRACE_START(), TRACE_DUR());
             """),
        out=Csv("""
          "awake_dur","awake_utilization","awake_unnormalized_utilization"
          7814964417,22.490000,89.960000
            """))

  def test_cpu_cycles_per_cpu(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.system;

             SELECT
              cpu,
              millicycles,
              megacycles,
              runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_cpu;
             """),
        out=Csv("""
          "cpu","millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
          0,4007488375822,4007,2260291804,930000,1803000,1772996
          1,3985923237512,3985,2247149674,930000,1803000,1773769
          2,4047926756581,4047,2276274170,930000,1803000,1778312
          3,3992276081242,3992,2248956757,930000,1803000,1775168
          4,5134318459625,5134,2203887266,553000,2348000,2329665
          5,5615703220380,5615,2438499077,553000,2348000,2302934
          6,4715590442538,4715,1737264802,500000,2850000,2714377
          7,4594701918170,4594,1719270548,500000,2850000,2672472
            """))

  def test_cpu_cycles_per_cpu_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.system;

             SELECT
              cpu,
              millicycles,
              megacycles,
              runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_cpu_in_interval(TRACE_START(), TRACE_DUR() / 10);
             """),
        out=Csv("""
          "cpu","millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
          0,27811901835,27,50296201,614400,1708800,665261
          1,2893791427,2,4709947,614400,614400,614523
          2,177750720,0,3718178,864000,864000,867076
          3,752843306,0,17468751,614400,864000,640717
            """))

  def test_cpu_cycles_per_thread(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.thread;

             SELECT
              utid,
              millicycles,
              megacycles,
              runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_thread
             WHERE utid < 10
             """),
        out=Csv("""
        "utid","millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
        1,39042295612,39,28747861,614400,1708800,1358134
        2,286312857,0,167552,1708800,1708800,1714448
        8,124651656403,124,99592232,614400,1708800,1251623
            """))

  def test_cpu_cycles_per_thread_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.thread;

             SELECT
              utid,
              millicycles,
              megacycles,
              runtime,
              awake_runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_thread_in_interval(TRACE_START(), TRACE_DUR() / 10)
             WHERE utid < 100
             """),
        out=Csv("""
            "utid","millicycles","megacycles","runtime","awake_runtime","min_freq","max_freq","avg_freq"
            1,1226879384,1,1996874,1996874,614400,614400,614669
            14,1247778191,1,2446930,2446930,614400,614400,614669
            15,1407232193,1,2384063,2384063,614400,614400,614511
            16,505278870,0,1142238,1142238,614400,614400,614694
            30,29888102,0,48646,48646,614400,614400,622668
            37,"[NULL]","[NULL]",222814,222814,"[NULL]","[NULL]","[NULL]"
            38,"[NULL]","[NULL]",2915520,2915520,"[NULL]","[NULL]","[NULL]"
            45,"[NULL]","[NULL]",2744688,2744688,"[NULL]","[NULL]","[NULL]"
            54,"[NULL]","[NULL]",8614114,8614114,"[NULL]","[NULL]","[NULL]"
            61,151616101,0,246771,246771,614400,614400,616325
            62,58740000,0,8307552,8307552,1708800,1708800,1727647
            92,243675648,0,962397,962397,864000,864000,864098
            """))

  def test_cpu_cycles_per_thread_per_cpu(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.thread;

             SELECT
              utid,
              cpu,
              millicycles,
              megacycles,
              runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_thread_per_cpu
             WHERE utid < 10
             """),
        out=Csv("""
        "utid","cpu","millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
        1,0,21613219642,21,17794320,614400,1708800,1214635
        1,1,2497607711,2,1461615,1708800,1708800,1709519
        1,2,4364824719,4,3151458,614400,1708800,1385218
        1,3,10566643540,10,6340468,1363200,1708800,1666663
        2,3,286312857,0,167552,1708800,1708800,1714448
        8,0,46170039382,46,43358479,614400,1708800,1064856
        8,1,14938296493,14,9838169,614400,1708800,1518428
        8,2,47599704832,47,35501050,614400,1708800,1340798
        8,3,15943615696,15,10894534,614400,1708800,1463522
        """))

  def test_cpu_cycles_per_thread_per_cpu_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
           INCLUDE PERFETTO MODULE linux.cpu.utilization.thread;

           SELECT
            utid,
            cpu,
            millicycles,
            megacycles,
            runtime,
            min_freq,
            max_freq,
            avg_freq
           FROM cpu_cycles_per_thread_per_cpu_in_interval(TRACE_START(), TRACE_DUR() / 10)
           WHERE utid < 100
           """),
        out=Csv("""
          "utid","cpu","millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
          1,0,1226879384,1,1996874,614400,614400,614669
          14,0,1133538499,1,2049326,614400,614400,614717
          14,1,114239692,0,185937,614400,614400,617511
          14,2,"[NULL]","[NULL]",211667,"[NULL]","[NULL]","[NULL]"
          15,0,980352605,0,1595626,614400,614400,614641
          15,1,426879588,0,694791,614400,614400,615100
          15,3,"[NULL]","[NULL]",93646,"[NULL]","[NULL]","[NULL]"
          16,0,505278870,0,822394,614400,614400,614694
          16,2,"[NULL]","[NULL]",319844,"[NULL]","[NULL]","[NULL]"
          30,1,29888102,0,48646,614400,614400,622668
          37,0,"[NULL]","[NULL]",157397,"[NULL]","[NULL]","[NULL]"
          37,2,"[NULL]","[NULL]",65417,"[NULL]","[NULL]","[NULL]"
          38,2,"[NULL]","[NULL]",2915520,"[NULL]","[NULL]","[NULL]"
          45,0,"[NULL]","[NULL]",2690990,"[NULL]","[NULL]","[NULL]"
          45,3,"[NULL]","[NULL]",53698,"[NULL]","[NULL]","[NULL]"
          54,0,"[NULL]","[NULL]",3688906,"[NULL]","[NULL]","[NULL]"
          54,3,"[NULL]","[NULL]",4925208,"[NULL]","[NULL]","[NULL]"
          61,0,151616101,0,246771,614400,614400,616325
          62,0,58740000,0,34375,1708800,1708800,1727647
          62,3,"[NULL]","[NULL]",8273177,"[NULL]","[NULL]","[NULL]"
          92,0,243675648,0,962397,864000,864000,864098
          """))

  def test_cpu_thread_utilization_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
              INCLUDE PERFETTO MODULE linux.cpu.utilization.thread;

              SELECT
                upid,
                utid,
                thread_name,
                awake_dur,
                awake_utilization,
                awake_unnormalized_utilization
              FROM cpu_thread_utilization_in_interval(TRACE_START(), TRACE_DUR())
              WHERE thread_name LIKE 'kswapd%'
              """),
        out=Csv("""
            "upid","utid","thread_name","awake_dur","awake_utilization","awake_unnormalized_utilization"
            62,62,"kswapd0",125991305,0.362560,1.450240
              """))

  def test_cpu_cycles_per_process(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.process;

             SELECT
              upid,
              millicycles,
              megacycles,
              runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_process
             WHERE upid < 10
             """),
        out=Csv("""
        "upid","millicycles","megacycles","runtime","min_freq","max_freq","avg_freq"
        1,79550724630,79,56977346,614400,1708800,1396190
        2,286312857,0,167552,1708800,1708800,1714448
        8,124651656403,124,99592232,614400,1708800,1251623
            """))

  def test_cpu_cycles_per_process_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.process;

             SELECT
              upid,
              millicycles,
              megacycles,
              runtime,
              awake_runtime,
              min_freq,
              max_freq,
              avg_freq
             FROM cpu_cycles_per_process_in_interval(TRACE_START(), TRACE_DUR() / 10)
             WHERE upid < 30;
             """),
        out=Csv("""
          "upid","millicycles","megacycles","runtime","awake_runtime","min_freq","max_freq","avg_freq"
          1,2163648305,2,3521563,3521563,614400,614400,614498
          14,1247778191,1,2446930,2446930,614400,614400,614669
          15,1407232193,1,2384063,2384063,614400,614400,614511
          16,505278870,0,1142238,1142238,614400,614400,614694
            """))

  def test_cpu_process_utilization_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.process;

             SELECT
              upid,
              process_name,
              awake_dur,
              awake_utilization,
              awake_unnormalized_utilization
             FROM cpu_process_utilization_in_interval(TRACE_START(), TRACE_DUR())
             WHERE process_name LIKE 'kswapd%';
             """),
        out=Csv("""
          "upid","process_name","awake_dur","awake_utilization","awake_unnormalized_utilization"
          62,"kswapd0",125991305,0.362560,1.450240
            """))

  def test_cpu_cycles_per_thread_slice(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.slice;

             SELECT
              id,
              utid,
              millicycles,
              megacycles
             FROM cpu_cycles_per_thread_slice
             WHERE millicycles IS NOT NULL
             LIMIT 10
             """),
        out=Csv("""
        "id","utid","millicycles","megacycles"
        110,17,13022368,0
        121,17,9618704,0
        125,6,6375728,0
        126,6,8699728,0
        128,6,5565648,0
        129,6,5565648,0
        146,24,6916224,0
        151,26,5296064,0
        203,17,150060016,0
        214,6,7132688,0
            """))

  def test_cpu_cycles_per_thread_slice_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.utilization.slice;

             SELECT
              id,
              utid,
              millicycles,
              megacycles
             FROM cpu_cycles_per_thread_slice_in_interval(TRACE_START(), TRACE_DUR() / 10)
             WHERE millicycles IS NOT NULL
             LIMIT 10
             """),
        out=Csv("""
        "id","utid","millicycles","megacycles"
        110,17,13022368,0
        121,17,9618704,0
        125,6,6375728,0
        126,6,8699728,0
        128,6,5565648,0
        129,6,5565648,0
        146,24,6916224,0
        151,26,5296064,0
        203,17,150060016,0
        214,6,7132688,0
            """))

  # Test CPU frequency counter grouping.
  def test_cpu_eos_counters_freq(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
          INCLUDE PERFETTO MODULE linux.cpu.frequency;
          select
            freq,
            cpu,
            sum(dur) as dur
          from cpu_frequency_counters
          GROUP BY freq, cpu
        """),
        out=Csv("""
          "freq","cpu","dur"
          614400,0,4755967239
          614400,1,4755971561
          614400,2,4755968228
          614400,3,4755964320
          864000,0,442371195
          864000,1,442397134
          864000,2,442417916
          864000,3,442434530
          1363200,0,897122398
          1363200,1,897144167
          1363200,2,897180154
          1363200,3,897216772
          1708800,0,2553979530
          1708800,1,2553923073
          1708800,2,2553866772
          1708800,3,2553814688
        """))

  # Test CPU idle state counter grouping.
  def test_cpu_eos_counters_idle(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE linux.cpu.idle;
             select
               idle,
               cpu,
               sum(dur) as dur
             from cpu_idle_counters
             GROUP BY idle, cpu
             """),
        out=Csv("""
          "idle","cpu","dur"
          -1,0,2839828332
          -1,1,1977033843
          -1,2,1800498713
          -1,3,1884366297
          0,0,1833971336
          0,1,2285260950
          0,2,1348416182
          0,3,1338508968
          1,0,4013820433
          1,1,4386917600
          1,2,5532102915
          1,3,5462026920
        """))

  def test_linux_cpu_idle_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event: {
                timestamp: 200000000000
                pid: 2
                cpu_frequency: {
                  state : 1704000
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200000000000
                pid: 2
                cpu_idle: {
                  state: 4294967295
                  cpu_id: 0
                }
              }
              event {
                timestamp: 200001000000
                pid: 2
                cpu_idle: {
                  state : 1
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200002000000
                pid  : 2
                cpu_idle: {
                  state : 4294967295
                  cpu_id: 0
                }
              }
              event {
                timestamp: 200003000000
                pid: 2
                cpu_idle: {
                  state : 1
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200004000000
                pid: 2
                cpu_idle: {
                  state : 4294967295
                  cpu_id: 0
                }
              }
              event: {
                timestamp: 200005000000
                pid: 2
                cpu_frequency: {
                  state: 300000
                  cpu_id: 0
                }
              }
            }
            trusted_uid: 9999
            trusted_packet_sequence_id: 2
          }
         """),
        query="""
         INCLUDE PERFETTO MODULE linux.cpu.idle_stats;
         SELECT * FROM cpu_idle_stats;
         """,
        out=Csv("""
         "cpu","state","count","dur","avg_dur","idle_percent"
         0,2,2,2000000,1000000,40.000000
         """))

  def test_linux_per_cpu_idle_time_in_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000000
              }
            }
          }
          timestamp: 200000000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 1
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000000
              }
            }
          }
          timestamp: 200000000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000100
              }
            }
          }
          timestamp: 200001000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 1
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000100
              }
            }
          }
          timestamp: 200001000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000200
              }
            }
          }
          timestamp: 200002000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 1
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000200
              }
            }
          }
          timestamp: 200002000000
          trusted_packet_sequence_id: 2
        }
         """),
        query="""
         INCLUDE PERFETTO MODULE linux.cpu.idle_time_in_state;
         SELECT * FROM linux_per_cpu_idle_time_in_state_counters;
         """,
        out=Csv("""
          "ts","machine_id","state","cpu","idle_percentage","total_residency","time_slice"
          200001000000,"[NULL]","C8",0,10.000000,100.000000,1000
          200002000000,"[NULL]","C8",0,10.000000,100.000000,1000
          200001000000,"[NULL]","C8",1,10.000000,100.000000,1000
          200002000000,"[NULL]","C8",1,10.000000,100.000000,1000
          200001000000,"[NULL]","C0",0,90.000000,900.000000,1000
          200001000000,"[NULL]","C0",1,90.000000,900.000000,1000
          200002000000,"[NULL]","C0",0,90.000000,900.000000,1000
          200002000000,"[NULL]","C0",1,90.000000,900.000000,1000
         """))

  def test_linux_cpu_idle_time_in_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000000
              }
            }
          }
          timestamp: 200000000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000100
              }
            }
          }
          timestamp: 200001000000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 1000200
              }
            }
          }
          timestamp: 200002000000
          trusted_packet_sequence_id: 2
        }
         """),
        query="""
         INCLUDE PERFETTO MODULE linux.cpu.idle_time_in_state;
         SELECT * FROM linux_cpu_idle_time_in_state_counters;
         """,
        out=Csv("""
          "ts","machine_id","state","idle_percentage","total_residency","time_slice"
          200001000000,"[NULL]","C0",90.000000,900.000000,1000
          200001000000,"[NULL]","C8",10.000000,100.000000,1000
          200002000000,"[NULL]","C0",90.000000,900.000000,1000
          200002000000,"[NULL]","C8",10.000000,100.000000,1000
         """))
