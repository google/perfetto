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
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class ProcessTracking(TestSuite):
  # Tests for the core process and thread tracking logic. Smoke tests
  def test_process_tracking(self):
    return DiffTestBlueprint(
        trace=Path('synth_process_tracking.py'),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        10,10,"process1","p1-t0"
        11,"[NULL]","[NULL]","p1-t1"
        12,10,"process1","p1-t2"
        20,20,"process_2","p2-t0"
        21,20,"process_2","p2-t1"
        22,20,"process_2","p2-t2"
        30,30,"process_3","p3-t0"
        31,30,"process_3","p3-t1"
        31,40,"process_4","p4-t1"
        32,30,"process_3","p3-t2"
        33,30,"process_3","p3-t3"
        34,30,"process_3","p3-t4"
        40,40,"process_4","p4-t0"
        """))

  # Short lived threads/processes
  def test_process_tracking_process_tracking_short_lived_1(self):
    return DiffTestBlueprint(
        trace=Path('process_tracking_short_lived_1.py'),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        10,10,"parent","parent"
        11,11,"child","child"
        """))

  def test_process_tracking_process_tracking_short_lived_2(self):
    return DiffTestBlueprint(
        trace=Path('process_tracking_short_lived_2.py'),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        10,10,"parent","parent"
        11,11,"true_name","true_name"
        """))

  # Process uid handling
  def test_process_tracking_uid(self):
    return DiffTestBlueprint(
        trace=Path('synth_process_tracking.py'),
        query="""
        SELECT pid, uid
        FROM process
        ORDER BY pid;
        """,
        out=Csv("""
        "pid","uid"
        0,"[NULL]"
        10,1001
        20,1002
        30,"[NULL]"
        40,"[NULL]"
        """))

  # Tracking across execs
  def test_process_tracking_process_tracking_exec(self):
    return DiffTestBlueprint(
        trace=Path('process_tracking_exec.py'),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        10,10,"parent","parent"
        11,11,"true_process_name","true_name"
        """))

  # Tracking parent threads
  def test_process_parent_pid_process_parent_pid_tracking_1(self):
    return DiffTestBlueprint(
        trace=Path('process_parent_pid_tracking_1.py'),
        query="""
        SELECT
          child.pid AS child_pid,
          parent.pid AS parent_pid
        FROM process AS child
        JOIN process AS parent
          ON child.parent_upid = parent.upid
        ORDER BY child_pid;
        """,
        out=Csv("""
        "child_pid","parent_pid"
        10,0
        20,10
        """))

  def test_process_parent_pid_process_parent_pid_tracking_2(self):
    return DiffTestBlueprint(
        trace=Path('process_parent_pid_tracking_2.py'),
        query="""
        SELECT
          child.pid AS child_pid,
          parent.pid AS parent_pid
        FROM process AS child
        JOIN process AS parent
          ON child.parent_upid = parent.upid
        ORDER BY child_pid;
        """,
        out=Csv("""
        "child_pid","parent_pid"
        10,0
        20,10
        """))

  # Tracking thread reuse
  def test_process_tracking_reused_thread_print(self):
    return DiffTestBlueprint(
        trace=Path('reused_thread_print.py'),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        10,10,"parent","[NULL]"
        11,11,"short_lived","[NULL]"
        11,10,"parent","true_name"
        """))

  # TODO(lalitm): move this out of this folder.
  def test_slice_with_pid_sde_tracing_mark_write(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 403
              sde_tracing_mark_write {
                pid: 403
                trace_name: "test_event"
                trace_begin: 1
              }
            }
            event {
              timestamp: 101
              pid: 403
              sde_tracing_mark_write {
                pid: 403
                trace_name: "test_event"
                trace_begin: 0
              }
            }
          }
        }
        """),
        query="""
        SELECT s.name, dur, tid, pid
        FROM slice s
        JOIN thread_track t ON s.track_id = t.id
        JOIN thread USING(utid)
        LEFT JOIN process USING(upid);
        """,
        out=Csv("""
        "name","dur","tid","pid"
        "test_event",1,403,403
        """))

  # Check that a <...> thread name doesn't overwrite a useful thread name
  def test_unknown_thread_name_tracking(self):
    return DiffTestBlueprint(
        trace=Path('unknown_thread_name.systrace'),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        19999,"[NULL]","[NULL]","real_name"
        """))

  def test_process_tracking_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('synth_process_tracking.py'),
        trace_modifier=TraceInjector(['ftrace_events', 'process_tree'],
                                     {'machine_id': 1001}),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname,
               thread.machine_id as tmachine, process.machine_id as pmachine
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname","tmachine","pmachine"
        10,10,"process1","p1-t0",1,1
        11,"[NULL]","[NULL]","p1-t1",1,"[NULL]"
        12,10,"process1","p1-t2",1,1
        20,20,"process_2","p2-t0",1,1
        21,20,"process_2","p2-t1",1,1
        22,20,"process_2","p2-t2",1,1
        30,30,"process_3","p3-t0",1,1
        31,30,"process_3","p3-t1",1,1
        31,40,"process_4","p4-t1",1,1
        32,30,"process_3","p3-t2",1,1
        33,30,"process_3","p3-t3",1,1
        34,30,"process_3","p3-t4",1,1
        40,40,"process_4","p4-t0",1,1
        """))

  def test_process_stats_process_runtime(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          first_packet_on_sequence: true
          timestamp: 1088821452006028
          incremental_state_cleared: true
          process_tree {
            processes {
              pid: 9301
              ppid: 9251
              uid: 304336
              nspid: 4
              nspid: 1
              cmdline: "/bin/command"
              process_start_from_boot: 157620000000
            }
            collection_end_timestamp: 1088821520810204
          }
          trusted_uid: 304336
          trusted_packet_sequence_id: 3
          trusted_pid: 1137063
          previous_packet_dropped: true
        }
        packet {
          timestamp: 1088821520899054
          process_stats {
            processes {
              pid: 9301
              runtime_user_mode: 16637390000000
              runtime_kernel_mode: 1327800000000
              vm_size_kb: 1188971644
              vm_locked_kb: 0
              vm_hwm_kb: 1180568
              vm_rss_kb: 1100672
              rss_anon_kb: 1045332
              rss_file_kb: 46848
              rss_shmem_kb: 8492
              vm_swap_kb: 163936
              oom_score_adj: 300
            }
            collection_end_timestamp: 1088821539659978
          }
          trusted_uid: 304336
          trusted_packet_sequence_id: 3
          trusted_pid: 1137063
        }
        packet {
          timestamp: 1088821786436938
          process_stats {
            processes {
              pid: 9301
              runtime_user_mode: 16638280000000
              runtime_kernel_mode: 1327860000000
              vm_size_kb: 1188979836
              vm_locked_kb: 0
              vm_hwm_kb: 1180568
              vm_rss_kb: 895428
              rss_anon_kb: 832028
              rss_file_kb: 46848
              rss_shmem_kb: 16552
              vm_swap_kb: 163936
              oom_score_adj: 300
            }
            collection_end_timestamp: 1088821817629747
          }
          trusted_uid: 304336
          trusted_packet_sequence_id: 3
          trusted_pid: 1137063
        }
        """),
        query="""
        select c.ts, c.value, pct.name, p.pid, p.start_ts, p.cmdline
        from counter c
          join process_counter_track pct on (c.track_id = pct.id)
          join process p using (upid)
        where pct.name in ("runtime.user_ns", "runtime.kernel_ns")
          and p.pid = 9301
        order by ts asc, pct.name asc
        """,
        out=Csv("""
        "ts","value","name","pid","start_ts","cmdline"
        1088821520899054,1327800000000.000000,"runtime.kernel_ns",9301,157620000000,"/bin/command"
        1088821520899054,16637390000000.000000,"runtime.user_ns",9301,157620000000,"/bin/command"
        1088821786436938,1327860000000.000000,"runtime.kernel_ns",9301,157620000000,"/bin/command"
        1088821786436938,16638280000000.000000,"runtime.user_ns",9301,157620000000,"/bin/command"
        """))

  # Distinguish set-to-zero process age (can happen for kthreads) from unset
  # process age.
  def test_process_age_optionality(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          first_packet_on_sequence: true
          timestamp: 1088821452006028
          incremental_state_cleared: true
          process_tree {
            processes {
              pid: 2
              ppid: 0
              uid: 0
              cmdline: "kthreadd"
              process_start_from_boot: 0
            }
            processes {
              pid: 68
              ppid: 2
              uid: 0
              cmdline: "ksoftirqd/7"
              process_start_from_boot: 10000000
            }
            processes {
              pid: 9301
              ppid: 9251
              uid: 304336
              cmdline: "no_age_field"
            }
            collection_end_timestamp: 1088821520810204
          }
          trusted_uid: 304336
          trusted_packet_sequence_id: 3
          trusted_pid: 1137063
          previous_packet_dropped: true
        }
        """),
        query="""
        select p.pid, p.start_ts, p.cmdline
        from process p
        where pid in (2, 68, 9301)
        order by pid asc;
        """,
        out=Csv("""
        "pid","start_ts","cmdline"
        2,0,"kthreadd"
        68,10000000,"ksoftirqd/7"
        9301,"[NULL]","no_age_field"
        """))
