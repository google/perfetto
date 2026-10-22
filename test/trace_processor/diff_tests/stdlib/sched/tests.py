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

from python.generators.diff_tests.testing import Path, DataPath, TextProto
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class StdlibSched(TestSuite):

  def test_runnable_thread_count(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
      SELECT * FROM sched_runnable_thread_count;
      """,
        out=Csv("""
      "ts","machine_id","runnable_thread_count"
      1,0,1
      50,0,1
      100,0,2
      115,0,2
      120,0,2
      170,0,3
      250,0,2
      390,0,2
      """))

  def test_active_cpu_count(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;

      SELECT * FROM sched_active_cpu_count;
      """,
        out=Csv("""
      "ts","machine_id","active_cpu_count"
      1,0,1
      50,0,2
      100,0,2
      115,0,2
      120,0,2
      170,0,1
      250,0,2
      390,0,2
      """))

  def test_thread_level_parallelism_is_partitioned_by_machine(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 0
              sched_switch {
                prev_comm: "swapper/0"
                prev_pid: 0
                prev_prio: 120
                prev_state: 0
                next_comm: "shared_pid"
                next_pid: 10
                next_prio: 120
              }
            }
            event {
              timestamp: 200
              pid: 10
              sched_switch {
                prev_comm: "shared_pid"
                prev_pid: 10
                prev_prio: 120
                prev_state: 0
                next_comm: "swapper/0"
                next_pid: 0
                next_prio: 120
              }
            }
            event {
              timestamp: 400
              pid: 0
              sched_switch {
                prev_comm: "swapper/0"
                prev_pid: 0
                prev_prio: 120
                prev_state: 0
                next_comm: "shared_pid"
                next_pid: 10
                next_prio: 120
              }
            }
          }
        }
        packet {
          machine_id: 1001
          remote_clock_sync {
            synced_clocks {
              client_clocks { clocks { clock_id: 6 timestamp: 0 } }
              host_clocks { clocks { clock_id: 6 timestamp: 0 } }
            }
          }
        }
        packet {
          machine_id: 1001
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 0
              sched_switch {
                prev_comm: "swapper/0"
                prev_pid: 0
                prev_prio: 120
                prev_state: 0
                next_comm: "shared_pid"
                next_pid: 10
                next_prio: 120
              }
            }
            event {
              timestamp: 250
              pid: 10
              sched_switch {
                prev_comm: "shared_pid"
                prev_pid: 10
                prev_prio: 120
                prev_state: 0
                next_comm: "swapper/0"
                next_pid: 0
                next_prio: 120
              }
            }
            event {
              timestamp: 350
              pid: 0
              sched_switch {
                prev_comm: "swapper/0"
                prev_pid: 0
                prev_prio: 120
                prev_state: 0
                next_comm: "shared_pid"
                next_pid: 10
                next_prio: 120
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE sched.thread_level_parallelism;

        SELECT
          m.raw_id AS raw_machine_id,
          max(runnable_thread_count) AS max_runnable,
          (SELECT max(active_cpu_count)
           FROM sched_active_cpu_count a
           WHERE a.machine_id = r.machine_id) AS max_active_cpus
        FROM sched_runnable_thread_count r
        JOIN machine m ON m.id = r.machine_id
        GROUP BY r.machine_id
        ORDER BY r.machine_id;
        """,
        out=Csv("""
        "raw_machine_id","max_runnable","max_active_cpus"
        0,1,1
        1001,1,1
        """))

  def test_sched_time_in_state_for_thread(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_time_in_state_for_thread
        ORDER BY utid, state
        LIMIT 10;
        """,
        out=Csv("""
        "utid","total_runtime","state","time_in_state","percentage_in_state"
        1,27540674878,"D",596720,0
        1,27540674878,"R",1988438,0
        1,27540674878,"R+",2435415,0
        1,27540674878,"Running",23098223,0
        1,27540674878,"S",27512556082,99
        2,27761417087,"D",833039830,3
        2,27761417087,"R+",2931096,0
        2,27761417087,"Running",92350845,0
        2,27761417087,"S",26833095316,96
        3,29374171050,"R",140800325,0
        """))

  def test_sched_percentage_of_time_in_state(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_percentage_of_time_in_state
        ORDER BY utid
        LIMIT 10;
        """,
        out=Csv("""
        "utid","running","runnable","runnable_preempted","sleeping","uninterruptible_sleep","other"
        1,0,0,0,99,0,"[NULL]"
        2,0,"[NULL]",0,96,3,"[NULL]"
        3,5,0,0,93,"[NULL]","[NULL]"
        4,100,"[NULL]","[NULL]","[NULL]","[NULL]",0
        5,0,0,0,99,0,"[NULL]"
        6,0,"[NULL]",0,99,"[NULL]","[NULL]"
        7,0,0,0,99,"[NULL]","[NULL]"
        8,0,0,0,98,0,"[NULL]"
        9,0,"[NULL]","[NULL]",99,"[NULL]","[NULL]"
        10,0,"[NULL]",0,99,"[NULL]","[NULL]"
        """))

  def test_sched_time_in_state_for_thread_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_time_in_state_for_thread_in_interval(71039311397, 10000000000, 44);
        """,
        out=Csv("""
        "state","io_wait","blocked_function","dur"
        "S","[NULL]","[NULL]",9994400675
        "Running","[NULL]","[NULL]",4655524
        "D","[NULL]","[NULL]",563645
        "R+","[NULL]","[NULL]",380156
        """))

  def test_sched_time_in_state_and_cpu_for_thread_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE sched.time_in_state;

        SELECT *
        FROM sched_time_in_state_and_cpu_for_thread_in_interval(71039311397, 10000000000, 44);
        """,
        out=Csv("""
        "state","io_wait","cpu","blocked_function","dur"
        "S","[NULL]","[NULL]","[NULL]",9994400675
        "Running","[NULL]",2,"[NULL]",4655524
        "D","[NULL]","[NULL]","[NULL]",563645
        "R+","[NULL]","[NULL]","[NULL]",380156
        """))

  def test_sched_time_in_state_for_cpu_in_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
      INCLUDE PERFETTO MODULE sched.time_in_state;
      SELECT * FROM
      sched_time_in_state_for_cpu_in_interval(1, TRACE_START(), TRACE_DUR());
      """,
        out=Csv("""
        "end_state","dur"
        "D",311982601
        "DK",31103960
        "R",23230879715
        "R+",1148673560
        "S",3868233011
        "x",35240577
      """))

  def test_sched_previous_runnable_on_thread(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE sched.runnable;

        SELECT
          running.ts AS running_ts,
          running_thread.tid AS running_tid,
          prev_runnable.ts AS prev_runnable_ts,
          prev_runnable_thread.tid AS prev_runnable_tid,
          prev_wakeup.ts AS prev_wakeup_ts,
          prev_wakeup_thread.tid AS prev_wakeup_tid
        FROM sched_previous_runnable_on_thread s
        JOIN thread_state running ON running.id = s.id
        JOIN thread running_thread ON running_thread.utid = running.utid
        JOIN thread_state prev_runnable ON prev_runnable.id = s.prev_runnable_id
        JOIN thread prev_runnable_thread ON prev_runnable_thread.utid = prev_runnable.utid
        JOIN thread_state prev_wakeup ON prev_wakeup.id = s.prev_wakeup_runnable_id
        JOIN thread prev_wakeup_thread ON prev_wakeup_thread.utid = prev_wakeup.utid
        ORDER BY running.ts DESC, running_thread.tid DESC
        LIMIT 10;
        """,
        out=Csv("""
        "running_ts","running_tid","prev_runnable_ts","prev_runnable_tid","prev_wakeup_ts","prev_wakeup_tid"
        9610742069,509,9610595870,509,9610595870,509
        9610725508,509,9610595870,509,9610595870,509
        9610687789,509,9610595870,509,9610595870,509
        9610565596,2246,9609128381,2246,9609128381,2246
        9610462325,509,9608319340,509,9578107751,509
        9610366255,889,9610234867,889,9532969161,889
        9610258305,1469,9610253422,1469,9603595910,1469
        9610253422,893,9533011926,893,9533011926,893
        9610234867,1469,9603595910,1469,9603595910,1469
        9610202640,889,9610190108,889,9532969161,889
        """))

  def test_sched_latency(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE sched.latency;

        SELECT
          running.ts,
          thread.tid,
          latency.latency_dur,
          sched.cpu,
          sched.dur
        FROM sched_latency_for_running_interval latency
        JOIN thread_state running ON running.id = latency.thread_state_id
        JOIN thread ON thread.utid = latency.utid
        JOIN sched ON sched.id = latency.sched_id AND sched.utid = latency.utid
        ORDER BY running.ts DESC, thread.tid DESC
        LIMIT 10;
        """,
        out=Csv("""
        "ts","tid","latency_dur","cpu","dur"
        9610742069,509,91919,7,6999
        9610725508,509,91919,7,6633
        9610687789,509,91919,7,20507
        9610565596,2246,1437215,7,122193
        9610462325,509,826823,7,18229
        9610366255,889,131388,7,96070
        9610258305,1469,4883,7,107950
        9610253422,893,469849,7,4883
        9610234867,1469,670736,7,18555
        9610202640,889,12532,7,32227
        """))

  def test_sched_critical_path(self):
    return DiffTestBlueprint(
        trace=DataPath('api34_startup_cold.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        INCLUDE PERFETTO MODULE sched.critical_path;

        SELECT
          p.ready_ts,
          p.ts,
          p.dur,
          t.tid,
          rt.tid AS ready_tid,
          p.ready_type,
          p.ready_info,
          p.thread_name,
          p.process_name,
          p.running_dur,
          p.runnable_dur,
          p.sleeping_dur,
          p.uninterruptible_sleep_dur
        FROM sched_critical_path(
          (SELECT ts FROM android_startups WHERE package = 'com.android.systemui.people'),
          (SELECT dur FROM android_startups WHERE package = 'com.android.systemui.people'),
          (SELECT utid FROM thread WHERE tid = 28573)
        ) p
        JOIN thread t USING (utid)
        LEFT JOIN thread rt ON rt.utid = p.ready_utid;
        """,
        out=Csv("""
        "ready_ts","ts","dur","tid","ready_tid","ready_type","ready_info","thread_name","process_name","running_dur","runnable_dur","sleeping_dur","uninterruptible_sleep_dur"
        "[NULL]",17806781251694,16249431,2139,28838,"thread_ready","","binder:1719_6","system_server",13046143,3203288,0,0
        17806797501125,17806797757513,539225,1754,2139,"thread_ready","","android.anim","system_server",539225,0,0,0
        17806798296738,17806798435492,931884,3242,1754,"binder transaction async","AIDL::cpp::IDisplayEventConnection::requestNextVsync::cppClient","binder:598_4","/system/bin/surfaceflinger",931884,0,0,0
        17806799367376,17806799589626,134928,710,3242,"thread_ready","","appSf","/system/bin/surfaceflinger",134928,0,0,0
        17806799724554,17806799802760,114991,1818,710,"thread_ready","","binder:598_3","/system/bin/surfaceflinger",114991,0,0,0
        17806799917751,17806802325181,2637288,2139,1818,"binder reply","AIDL::cpp::ISurfaceComposerClient::createSurface::cppClient","binder:1719_6","system_server",2637288,0,0,0
        17806804962469,17806805002061,113647,1818,2139,"binder transaction","AIDL::cpp::ISurfaceComposerClient::createSurface::cppClient","binder:598_3","/system/bin/surfaceflinger",113647,0,0,0
        17806805115708,17806805896185,1247599,2139,1818,"binder reply","AIDL::cpp::ISurfaceComposerClient::createSurface::cppClient","binder:1719_6","system_server",1074218,173381,0,0
        17806807143784,17806807221217,570435,28422,2139,"binder transaction async","","binder:24955_8","com.google.android.apps.nexuslauncher",352702,217733,0,0
        17806807791652,17806807862534,1182780,24955,28422,"thread_ready","","s.nexuslauncher","com.google.android.apps.nexuslauncher",750732,432048,0,0
        17806809045314,17806809311509,353841,3674,24955,"binder transaction","","binder:1719_11","system_server",353841,0,0,0
        17806809665350,17806815300563,214885,3674,3674,"lock contention","Lock contention on a monitor lock (owner tid: 2139)","binder:1719_11","system_server",173991,40894,0,0
        17806815515448,17806815834987,284749,24955,3674,"binder reply","","s.nexuslauncher","com.google.android.apps.nexuslauncher",284749,0,0,0
        17806816119736,17806816599025,152018,4133,24955,"binder transaction","","binder:1719_16","system_server",152018,0,0,0
        17806816751043,17806817159613,1506917,4133,4133,"lock contention","Lock contention on a monitor lock (owner tid: 1754)","binder:1719_16","system_server",1506917,0,0,0
        17806818666530,17806818732244,669393,28592,4133,"binder transaction async","","binder:28573_3","com.android.systemui.people",669393,0,0,0
        17806819401637,17806819440049,710856,28573,28592,"thread_ready","","systemui.people","com.android.systemui.people",710856,0,0,0
        17806820150905,17806820532497,77759,3674,28573,"binder transaction","","binder:1719_11","system_server",77759,0,0,0
        17806820610256,17806824594061,267049,3674,3674,"lock contention","Lock contention on a monitor lock (owner tid: 4133)","binder:1719_11","system_server",267049,0,0,0
        17806824861110,17806824884710,234986,28573,3674,"binder reply","","systemui.people","com.android.systemui.people",234986,0,0,0
        17806825119696,17806825671535,140584,518,28573,"binder transaction","","servicemanager","/system/bin/servicemanager",140584,0,0,0
        17806825812119,17806826195379,51026,28573,518,"binder reply","","systemui.people","com.android.systemui.people",51026,0,0,0
        17806826246405,17806828421901,69377,3236,28573,"binder transaction","","binder:1719_B","system_server",69377,0,0,0
        17806828491278,17806828513657,544108,28573,3236,"binder reply","","systemui.people","com.android.systemui.people",544108,0,0,0
        17806829057765,17806829381496,102783,3236,28573,"binder transaction","","binder:1719_B","system_server",102783,0,0,0
        17806829484279,17806829617620,210083,3236,3236,"lock contention","Lock contention on a monitor lock (owner tid: 1754)","binder:1719_B","system_server",210083,0,0,0
        17806829827703,17806829891017,519613,28573,3236,"binder reply","","systemui.people","com.android.systemui.people",519613,0,0,0
        17806830410630,17806830452988,97331,3674,28573,"binder transaction","","binder:1719_11","system_server",97331,0,0,0
        17806830550319,17806830774033,77678,3674,3674,"lock contention","Lock contention on a monitor lock (owner tid: 1753)","binder:1719_11","system_server",77678,0,0,0
        17806830851711,17806830892848,250407,28573,3674,"binder reply","","systemui.people","com.android.systemui.people",250407,0,0,0
        17806831143255,17806831226955,133952,518,28573,"binder transaction","","servicemanager","/system/bin/servicemanager",133952,0,0,0
        17806831360907,17806831403387,41585,28573,518,"binder reply","","systemui.people","com.android.systemui.people",41585,0,0,0
        17806831444972,17806834131740,51676,3674,28573,"binder transaction","","binder:1719_11","system_server",51676,0,0,0
        17806834183416,17806834250474,4493449,28573,3674,"binder reply","","systemui.people","com.android.systemui.people",3286011,1207438,0,0
        17806838743923,17806838809474,131796,518,28573,"binder transaction","","servicemanager","/system/bin/servicemanager",131796,0,0,0
        17806838941270,17806838974554,168742,28573,518,"binder reply","","systemui.people","com.android.systemui.people",168742,0,0,0
        17806839143296,17806839157171,19247,3242,28573,"binder transaction","AIDL::cpp::ISurfaceComposer::createConnection::cppClient","binder:598_4","/system/bin/surfaceflinger",19247,0,0,0
        17806839176418,17806839210882,199056,28573,3242,"binder reply","AIDL::cpp::ISurfaceComposer::createConnection::cppClient","systemui.people","com.android.systemui.people",199056,0,0,0
        17806839409938,17806839422959,37842,28595,28573,"thread_ready","","RenderThread","com.android.systemui.people",37842,0,0,0
        17806839460801,17806839480535,269410,4269,28595,"binder transaction","AIDL::cpp::IHintManager::createHintSession::cppClient","binder:1719_1E","system_server",269410,0,0,0
        17806839749945,17806839796860,53060,719,4269,"binder transaction","AIDL::cpp::IPower::createHintSession::cppClient","android.hardwar","/vendor/bin/hw/android.hardware.power-service.pixel-libperfmgr",53060,0,0,0
        17806839849920,17806840449001,174682,4269,719,"binder reply","AIDL::cpp::IPower::createHintSession::cppClient","binder:1719_1E","system_server",174682,0,0,0
        17806840623683,17806840650417,27832,28595,4269,"binder reply","AIDL::cpp::IHintManager::createHintSession::cppClient","RenderThread","com.android.systemui.people",27832,0,0,0
        17806840678249,17806840695257,44068,28573,28595,"thread_ready","","systemui.people","com.android.systemui.people",44068,0,0,0
        17806840739325,17806845572129,5002075,28573,28573,"vsync","571743","systemui.people","com.android.systemui.people",4857218,144857,0,0
        17806850574204,17806850604030,409668,3236,28573,"binder transaction","","binder:1719_B","system_server",409668,0,0,0
        17806851013698,17806851040269,1713216,28573,3236,"binder reply","","systemui.people","com.android.systemui.people",1690471,22745,0,0
        17806852753485,17806888976182,2055989,28573,28573,"vsync","571765","systemui.people","com.android.systemui.people",2055989,0,0,0
        """))
