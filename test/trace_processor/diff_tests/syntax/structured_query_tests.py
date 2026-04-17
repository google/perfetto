#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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
from python.generators.diff_tests.testing import StructuredQuery


class StructuredQueryTests(TestSuite):

  def test_structured_query_simple_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 100
                pid: 10
                sched_switch {
                  prev_comm: "thread1"
                  prev_pid: 10
                  prev_prio: 120
                  prev_state: 1
                  next_comm: "thread2"
                  next_pid: 20
                  next_prio: 120
                }
              }
            }
          }
        """),
        query=StructuredQuery(
            query_id='simple_table',
            spec_file=DataPath('structured_query_simple.textproto')),
        out=Csv("""
          "ts","cpu"
          100,0
        """))

  def test_structured_query_with_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 100
                pid: 10
                sched_switch {
                  prev_comm: "thread1"
                  prev_pid: 10
                  prev_prio: 120
                  prev_state: 1
                  next_comm: "thread2"
                  next_pid: 20
                  next_prio: 120
                }
              }
              event {
                timestamp: 200
                pid: 20
                sched_switch {
                  prev_comm: "thread2"
                  prev_pid: 20
                  prev_prio: 120
                  prev_state: 1
                  next_comm: "thread3"
                  next_pid: 30
                  next_prio: 120
                }
              }
            }
          }
        """),
        query=StructuredQuery(
            query_id='with_filter',
            spec_file=DataPath('structured_query_with_filter.textproto')),
        out=Csv("""
          "ts","cpu"
          200,0
        """))

  def test_structured_query_textproto_simple(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 100
                pid: 10
                sched_switch {
                  prev_comm: "thread1"
                  prev_pid: 10
                  prev_prio: 120
                  prev_state: 1
                  next_comm: "thread2"
                  next_pid: 20
                  next_prio: 120
                }
              }
            }
          }
        """),
        query=StructuredQuery(
            query_id='simple_textproto',
            spec_textproto="""
              query {
                id: "simple_textproto"
                sql {
                  sql: "SELECT ts, cpu FROM ftrace_event"
                }
              }
            """),
        out=Csv("""
          "ts","cpu"
          100,0
        """))

  def test_structured_query_textproto_with_multiple_queries(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            ftrace_events {
              cpu: 0
              event {
                timestamp: 100
                pid: 10
                sched_switch {
                  prev_comm: "thread1"
                  prev_pid: 10
                  prev_prio: 120
                  prev_state: 1
                  next_comm: "thread2"
                  next_pid: 20
                  next_prio: 120
                }
              }
              event {
                timestamp: 200
                pid: 20
                sched_switch {
                  prev_comm: "thread2"
                  prev_pid: 20
                  prev_prio: 120
                  prev_state: 1
                  next_comm: "thread3"
                  next_pid: 30
                  next_prio: 120
                }
              }
            }
          }
        """),
        query=StructuredQuery(
            query_id='second_query',
            spec_textproto="""
              query {
                id: "first_query"
                sql {
                  sql: "SELECT ts, cpu FROM ftrace_event WHERE ts < 150"
                }
              }
              query {
                id: "second_query"
                sql {
                  sql: "SELECT ts, cpu FROM ftrace_event WHERE ts >= 150"
                }
              }
            """),
        out=Csv("""
          "ts","cpu"
          200,0
        """))
