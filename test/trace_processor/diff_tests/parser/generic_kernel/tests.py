#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class GenericKernelParser(TestSuite):

  def test_sched_switch_simple(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          end_state,
          priority,
          ucpu
        from sched_slice
        """,
        out=Csv("""
        "ts","dur","cpu","utid","end_state","priority","ucpu"
        360831239274,1000000000,0,1,"X",100,0
        """))

  def test_sched_switch_huge_tid(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 9023372036854775807
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 9023372036854775807
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          end_state,
          priority,
          ucpu
        from sched_slice
        """,
        out=Csv("""
        "ts","dur","cpu","utid","end_state","priority","ucpu"
        360831239274,1000000000,0,1,"X",100,0
        """))

  def test_sched_switch_thread_state_simple(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state 
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,1000000000,0,1,"Running",0
        361831239274,-1,"[NULL]",1,"X","[NULL]"
        """))

  # Testing scenario:
  #   start task1 -> start task2 -> close task1
  def test_sched_switch_interleaved(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          end_state,
          priority,
          ucpu
        from sched_slice
        """,
        out=Csv("""
        "ts","dur","cpu","utid","end_state","priority","ucpu"
        360831239274,1000000000,0,1,"X",100,0
        361831239274,-1,0,2,"[NULL]",100,0
        """))

  def test_sched_switch_interleaved_thread_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,1000000000,0,1,"Running",0
        361831239274,-1,"[NULL]",1,"X","[NULL]"
        361831239274,-1,0,2,"Running",0
        """))

  # Testing scenario:
  #   start task1 -> start task2 -> close task1
  # But the close ts doesn't align with start of task2
  def test_sched_switch_interleaved_mismatched(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          end_state,
          priority,
          ucpu
        from sched_slice
        """,
        out=Csv("""
        "ts","dur","cpu","utid","end_state","priority","ucpu"
        360831239274,1000000000,0,1,"[NULL]",100,0
        361831239274,-1,0,2,"[NULL]",100,0
        """))

  def test_sched_switch_interleaved_mismatched_thread_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 8
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,1000000000,0,1,"Running",0
        361831239274,1000000000,"[NULL]",1,"[NULL]","[NULL]"
        361831239274,-1,0,2,"Running",0
        362831239274,-1,"[NULL]",1,"X","[NULL]"
        """))

  def test_sched_switch_multiple_threads(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 1
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 2
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            comm: "task3"
            tid: 3
            state: 4
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          end_state,
          priority,
          ucpu
        from sched_slice
        """,
        out=Csv("""
        "ts","dur","cpu","utid","end_state","priority","ucpu"
        360831239274,1000000000,0,1,"[NULL]",100,0
        361831239274,-1,0,2,"[NULL]",100,0
        """))

  def test_sched_switch_multiple_threads_thread_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 1
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 2
            state: 3
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            comm: "task3"
            tid: 3
            state: 4
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,1000000000,0,1,"Running",0
        361831239274,-1,"[NULL]",1,"[NULL]","[NULL]"
        361831239274,-1,0,2,"Running",0
        362831239274,-1,"[NULL]",3,"S","[NULL]"
        """))

  def test_thread_state_created_and_dead(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 101
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 101
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,1000000000,"[NULL]",1,"Created","[NULL]"
        361831239274,-1,"[NULL]",1,"Z","[NULL]"
        """))

  def test_thread_created_and_dead(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        packet {
          timestamp: 363831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        """),
        query="""
        select
          utid,
          tid,
          name,
          start_ts,
          end_ts
        from thread
        """,
        out=Csv("""
        "utid","tid","name","start_ts","end_ts"
        0,0,"swapper","[NULL]","[NULL]"
        1,101,"task1",360831239274,362831239274
        2,102,"task2",361831239274,363831239274
        """))

  def test_thread_created_and_dead_huge_tid(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 9023372036854775807
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 8923372036854775807
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 9023372036854775807
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        packet {
          timestamp: 363831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 8923372036854775807
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        """),
        query="""
        select
          utid,
          tid,
          name,
          start_ts,
          end_ts
        from thread
        """,
        out=Csv("""
        "utid","tid","name","start_ts","end_ts"
        0,0,"swapper","[NULL]","[NULL]"
        1,9023372036854775807,"task1",360831239274,362831239274
        2,8923372036854775807,"task2",361831239274,363831239274
        """))

  def test_thread_state_created_and_destroyed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 101
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 101
            state: TASK_STATE_DESTROYED
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state 
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,1000000000,"[NULL]",1,"Created","[NULL]"
        361831239274,-1,"[NULL]",1,"X","[NULL]"
        """))

  def test_thread_created_and_destroyed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: TASK_STATE_DESTROYED
            prio: 100
          }
        }
        """),
        query="""
        select
          utid,
          tid,
          name,
          start_ts,
          end_ts
        from thread
        """,
        out=Csv("""
        "utid","tid","name","start_ts","end_ts"
        0,0,"swapper","[NULL]","[NULL]"
        1,101,"task1",360831239274,"[NULL]"
        """))

  def test_thread_only_destroyed_or_dead(self):
    # The DESTROYED event should be ignored
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 101
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            comm: "task2"
            tid: 102
            state: TASK_STATE_DESTROYED
            prio: 100
          }
        }
        """),
        query="""
        select
          utid,
          tid,
          name,
          start_ts,
          end_ts
        from thread
        """,
        out=Csv("""
        "utid","tid","name","start_ts","end_ts"
        0,0,"swapper","[NULL]","[NULL]"
        1,101,"task1","[NULL]",360831239274
        """))

  def test_thread_state_only_destroyed_or_dead(self):
    # The DESTROYED event should be ignored
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 101
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            comm: "task2"
            tid: 102
            state: TASK_STATE_DESTROYED
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: TASK_STATE_RUNNING
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        360831239274,-1,"[NULL]",1,"Z","[NULL]"
        362831239274,-1,0,2,"Running",0
        """))

  def test_thread_state_destroyed_after_dead(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 359831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 1
            state: TASK_STATE_RUNNING
            prio: 100
          }
        }
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 1
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 1
            state: TASK_STATE_DESTROYED
            prio: 100
          }
        }
        """),
        query="""
        select
          ts,
          dur,
          cpu,
          utid,
          state,
          ucpu
        from thread_state
        """,
        out=Csv("""
        "ts","dur","cpu","utid","state","ucpu"
        359831239274,1000000000,0,1,"Running",0
        360831239274,-1,"[NULL]",1,"Z","[NULL]"
        """))

  def test_error_stats_created_after_running(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 359831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 1
            state: TASK_STATE_RUNNING
            prio: 100
          }
        }
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 1
            state: TASK_STATE_CREATED
            prio: 100
          }
        }
        """),
        query="""
        select
          name,
          severity,
          source,
          value,
          description
        from stats
        where name = "generic_task_state_invalid_order"
        """,
        out=Csv(
            """
        "name","severity","source","value","description"
        "generic_task_state_invalid_order","error","analysis",1,""" +
            """"Invalid order of generic task state events. Should never happen."
        """))

  def test_error_stats_dead_after_destroyed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 359831239274
          generic_kernel_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 1
            state: TASK_STATE_RUNNING
            prio: 100
          }
        }
        packet {
          timestamp: 360831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 1
            state: TASK_STATE_DESTROYED
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_kernel_task_state_event {
            comm: "task1"
            tid: 1
            state: TASK_STATE_DEAD
            prio: 100
          }
        }
        """),
        query="""
        select
          name,
          severity,
          source,
          value,
          description
        from stats
        where name = "generic_task_state_invalid_order"
        """,
        out=Csv(
            """
        "name","severity","source","value","description"
        "generic_task_state_invalid_order","error","analysis",1,""" +
            """"Invalid order of generic task state events. Should never happen."
        """))
