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
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 2
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 7
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
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 2
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 7
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
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 2
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: 2
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 7
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
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 2
            prio: 100
          }
        }
        packet {
          timestamp: 361831239274
          generic_task_state_event {
            cpu: 0
            comm: "task2"
            tid: 102
            state: 2
            prio: 100
          }
        }
        packet {
          timestamp: 362831239274
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 7
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
        360831239274,2000000000,0,1,"Running",0
        361831239274,-1,0,2,"Running",0
        362831239274,-1,"[NULL]",1,"X","[NULL]"
        """))

  def test_thread_state_created(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 360831239274
          generic_task_state_event {
            cpu: 0
            comm: "task1"
            tid: 101
            state: 0
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
        360831239274,-1,"[NULL]",1,"Created","[NULL]"
        """))
