#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License"); # you may not use this file except in compliance with the License.  # You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Csv, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

# Tracepoints used for test trace:
#
# TRACE_EVENT(tid_track_example,
#
#     TP_PROTO(
#         char track_event_type,
#         const char *slice_name
#     ),
#
#     TP_ARGS(track_event_type, slice_name),
#     TP_STRUCT__entry(
#         __field(char, track_event_type)
#         __string(slice_name, slice_name)
#     ),
#     TP_fast_assign(
#         __entry->track_event_type = track_event_type;
#         __assign_str(slice_name);
#     ),
#     TP_printk(
#         "type=%c slice_name=%s",
#         __entry->track_event_type,
#         __get_str(slice_name)
#     )
# );
#
# TRACE_EVENT(tgid_track_example,
#
#     TP_PROTO(
#         char track_event_type,
#         const char *slice_name,
#         int scope_tgid
#     ),
#
#     TP_ARGS(track_event_type, slice_name, scope_tgid),
#     TP_STRUCT__entry(
#         __field(char, track_event_type)
#         __string(slice_name, slice_name)
#         __field(int, scope_tgid)
#     ),
#     TP_fast_assign(
#         __entry->track_event_type = track_event_type;
#         __assign_str(slice_name);
#         __entry->scope_tgid = scope_tgid;
#     ),
#     TP_printk(
#         "type=%c slice_name=%s tgid=%d",
#         __entry->track_event_type,
#         __get_str(slice_name),
#         __entry->scope_tgid
#     )
# );
#
#
# TRACE_EVENT(tgid_counter_example,
#
#     TP_PROTO(
#         u64 counter_value,
#         int scope_tgid
#     ),
#
#     TP_ARGS(counter_value, scope_tgid),
#     TP_STRUCT__entry(
#         __field(u64, counter_value)
#         __field(int, scope_tgid)
#     ),
#     TP_fast_assign(
#         __entry->counter_value = counter_value;
#         __entry->scope_tgid = scope_tgid;
#     ),
#     TP_printk(
#         "counter_value=%llu tgid=%d",
#         (unsigned long long)__entry->counter_value,
#         __entry->scope_tgid
#     )
# );
#
# TRACE_EVENT(cpu_counter_example,
#
#     TP_PROTO(
#         u64 counter_value,
#         int scope_cpu
#     ),
#
#     TP_ARGS(counter_value, scope_cpu),
#     TP_STRUCT__entry(
#         __field(u64, counter_value)
#         __field(int, scope_cpu)
#     ),
#     TP_fast_assign(
#         __entry->counter_value = counter_value;
#         __entry->scope_cpu = scope_cpu;
#     ),
#     TP_printk(
#         "counter_value=%llu cpu=%d",
#         (unsigned long long)__entry->counter_value,
#         __entry->scope_cpu
#     )
# );
#
# TRACE_EVENT(global_counter_example,
#
#     TP_PROTO(
#         u64 counter_value,
#         int scope_custom,
#         const char *track_name
#     ),
#
#     TP_ARGS(counter_value, scope_custom, track_name),
#     TP_STRUCT__entry(
#         __field(u64, counter_value)
#         __field(int, scope_custom)
#         __string(track_name, track_name)
#     ),
#     TP_fast_assign(
#         __entry->counter_value = counter_value;
#         __entry->scope_custom = scope_custom;
#         __assign_str(track_name);
#     ),
#     TP_printk(
#         "track_name=%s counter_value=%llu scope=%d",
#         __get_str(track_name),
#         (unsigned long long)__entry->counter_value,
#         __entry->scope_custom
#     )
# );


class KernelTrackevent(TestSuite):

  # Five sets of the following slice stacks, spread over three thread tracks.
  #
  # [ outer slice ......... ]
  #     [ nested slice ]
  #            V
  #            instant
  def test_thread_trackevent(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_kernel_trackevent.pftrace'),
        query="""
        WITH
          kernel_tracks AS (
            SELECT
              id,
              name,
              extract_arg(dimension_arg_set_id, 'utid') AS utid
            FROM track
            WHERE
              type = 'kernel_trackevent_thread_slice'
          ),
          kernel_thread_tracks AS (
            SELECT
              kernel_tracks.id,
              row_number() OVER (ORDER BY tid) AS track_number,
              kernel_tracks.name,
              tid
            FROM kernel_tracks
            JOIN thread
              USING (utid)
          )
        SELECT
          s.ts,
          s.dur,
          s.name AS slice_name,
          kt.name AS track_name,
          kt.tid,
          kt.track_number
        FROM slice AS s
        JOIN kernel_thread_tracks AS kt
          ON s.track_id = kt.id
        ORDER BY
          tid,
          ts;
        """,
        out=Csv("""
        "ts","dur","slice_name","track_name","tid","track_number"
        2525300090812,402485,"outer slice","tid_track_example",0,1
        2525300191715,200808,"nested slice","tid_track_example",0,1
        2525300292114,0,"instant","tid_track_example",0,1
        2525508006185,401842,"outer slice","tid_track_example",537,2
        2525508106806,200817,"nested slice","tid_track_example",537,2
        2525508207214,0,"instant","tid_track_example",537,2
        2525612017975,401859,"outer slice","tid_track_example",537,2
        2525612118618,200824,"nested slice","tid_track_example",537,2
        2525612219032,0,"instant","tid_track_example",537,2
        2525717006031,401958,"outer slice","tid_track_example",537,2
        2525717106644,200939,"nested slice","tid_track_example",537,2
        2525717207140,0,"instant","tid_track_example",537,2
        2525404006348,401904,"outer slice","tid_track_example",538,3
        2525404106964,200828,"nested slice","tid_track_example",538,3
        2525404207388,0,"instant","tid_track_example",538,3
        """))

  # Five sets of the following slice stacks, spread over two process
  # tracks (swapper and a multithreaded userspace process).
  #
  # [ outer slice ......... ]
  #     [ nested slice ]
  #            V
  #            instant
  def test_process_trackevent(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_kernel_trackevent.pftrace'),
        query="""
        WITH
          kernel_tracks AS (
            SELECT
              id,
              name,
              extract_arg(dimension_arg_set_id, 'upid') AS upid
            FROM track
            WHERE
              type = 'kernel_trackevent_process_slice'
          ),
          kernel_process_tracks AS (
            SELECT
              kernel_tracks.id,
              row_number() OVER (ORDER BY pid) AS track_number,
              kernel_tracks.name,
              pid
            FROM kernel_tracks
            JOIN process
              USING (upid)
          )
        SELECT
          s.ts,
          s.dur,
          s.name AS slice_name,
          kt.name AS track_name,
          kt.pid,
          kt.track_number
        FROM slice AS s
        JOIN kernel_process_tracks AS kt
          ON s.track_id = kt.id
        ORDER BY
          pid,
          ts;
        """,
        out=Csv("""
        "ts","dur","slice_name","track_name","pid","track_number"
        2525300091405,402421,"outer slice","tgid_track_example",0,1
        2525300191893,200880,"nested slice","tgid_track_example",0,1
        2525300292292,0,"instant","tgid_track_example",0,1
        2525404006622,401788,"outer slice","tgid_track_example",535,2
        2525404107153,200890,"nested slice","tgid_track_example",535,2
        2525404207563,0,"instant","tgid_track_example",535,2
        2525508006407,401778,"outer slice","tgid_track_example",535,2
        2525508106986,200824,"nested slice","tgid_track_example",535,2
        2525508207390,0,"instant","tgid_track_example",535,2
        2525612018223,401768,"outer slice","tgid_track_example",535,2
        2525612118792,200838,"nested slice","tgid_track_example",535,2
        2525612219211,0,"instant","tgid_track_example",535,2
        2525717006252,401892,"outer slice","tgid_track_example",535,2
        2525717106886,200885,"nested slice","tgid_track_example",535,2
        2525717207332,0,"instant","tgid_track_example",535,2
        """))

  # Counter split over two process tracks (swapper and a multithreaded
  # userspace process).
  def test_process_counter(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_kernel_trackevent.pftrace'),
        query="""
        WITH
          kernel_tracks AS (
            SELECT
              id,
              name,
              extract_arg(dimension_arg_set_id, 'upid') AS upid
            FROM track
            WHERE
              type = 'kernel_trackevent_process_counter'
          ),
          kernel_process_tracks AS (
            SELECT
              kernel_tracks.id,
              row_number() OVER (ORDER BY pid) AS track_number,
              kernel_tracks.name,
              pid
            FROM kernel_tracks
            JOIN process
              USING (upid)
          )
        SELECT
          c.ts,
          c.value,
          kt.name AS track_name,
          kt.pid,
          kt.track_number
        FROM counter AS c
        JOIN kernel_process_tracks AS kt
          ON c.track_id = kt.id
        ORDER BY
          pid,
          ts;
        """,
        out=Csv("""
        "ts","value","track_name","pid","track_number"
        2525300088339,23049.000000,"tgid_counter_example",0,1
        2525404005171,23050.000000,"tgid_counter_example",535,2
        2525508004765,23051.000000,"tgid_counter_example",535,2
        2525612016703,23052.000000,"tgid_counter_example",535,2
        2525717004801,23053.000000,"tgid_counter_example",535,2
        """))

  # Counter split over two cpu tracks.
  def test_cpu_counter(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_kernel_trackevent.pftrace'),
        query="""
        WITH
          kernel_tracks AS (
            SELECT
              id,
              name,
              extract_arg(dimension_arg_set_id, 'cpu') AS cpu
            FROM track
            WHERE
              type = 'kernel_trackevent_cpu_counter'
          ),
          kernel_process_tracks AS (
            SELECT
              kernel_tracks.id,
              row_number() OVER (ORDER BY cpu) AS track_number,
              kernel_tracks.name,
              cpu
            FROM kernel_tracks
          )
        SELECT
          c.ts,
          c.value,
          kt.name AS track_name,
          kt.cpu,
          kt.track_number
        FROM counter AS c
        JOIN kernel_process_tracks AS kt
          ON c.track_id = kt.id
        ORDER BY
          cpu,
          ts;
        """,
        out=Csv("""
        "ts","value","track_name","cpu","track_number"
        2525404005752,23050.000000,"cpu_counter_example",2,1
        2525508005709,23051.000000,"cpu_counter_example",2,1
        2525612017380,23052.000000,"cpu_counter_example",2,1
        2525717005557,23053.000000,"cpu_counter_example",2,1
        2525300089434,23049.000000,"cpu_counter_example",3,2
        """))

  # Counter with a single global track using a custom scope.
  # Plus explicit track name taken from event payloads.
  def test_global_counter(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_kernel_trackevent.pftrace'),
        query="""
        WITH
          kernel_tracks AS (
            SELECT
              id,
              name,
              extract_arg(dimension_arg_set_id, 'scope') AS scope
            FROM track
            WHERE
              type = 'kernel_trackevent_custom_counter'
          ),
          kernel_process_tracks AS (
            SELECT
              kernel_tracks.id,
              row_number() OVER (ORDER BY scope) AS track_number,
              kernel_tracks.name,
              scope
            FROM kernel_tracks
          )
        SELECT
          c.ts,
          c.value,
          kt.name AS track_name,
          kt.scope,
          kt.track_number
        FROM counter AS c
        JOIN kernel_process_tracks AS kt
          ON c.track_id = kt.id
        ORDER BY
          scope,
          ts;
        """,
        out=Csv("""
        "ts","value","track_name","scope","track_number"
        2525300090298,23049.000000,"named counter",0,1
        2525404006023,23050.000000,"named counter",0,1
        2525508005931,23051.000000,"named counter",0,1
        2525612017670,23052.000000,"named counter",0,1
        2525717005780,23053.000000,"named counter",0,1
        """))
