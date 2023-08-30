#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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

from perfetto.trace_processor.api import TraceProcessor
from perfetto.trace_processor.api import TraceProcessorException


def compute_breakdown(tp: TraceProcessor,
                      start_ts=None,
                      end_ts=None,
                      process_name=None,
                      main_thread: bool = False):
  """For each userspace slice in the trace processor instance |tp|, computes
  the self-time of that slice grouping by process name, thread name
  and thread state.

  Args:
    tp: the trace processor instance to query.
    start_ts: optional bound to only consider slices after this ts
    end_ts: optional bound to only consider slices until this ts
    process_name: optional process name to filter for slices; specifying
        this argument can make computing the breakdown a lot faster.
    main_thread: whether to only consider slices on the main thread of each
        process.

  Returns:
    A Pandas dataframe containing the total self time taken by a slice stack
    broken down by process name, thread name and thread state.
  """
  bounds = tp.query('SELECT * FROM trace_bounds').as_pandas_dataframe()
  start_ts = start_ts if start_ts else bounds['start_ts'][0]
  end_ts = end_ts if end_ts else bounds['end_ts'][0]

  tp.query("""
    DROP VIEW IF EXISTS modded_names
  """)

  tp.query("""
    INCLUDE PERFETTO MODULE android.slices;
    CREATE VIEW modded_names AS
    SELECT
      slice.id,
      slice.depth,
      slice.stack_id,
      ANDROID_STANDARDIZE_SLICE_NAME(slice.name) AS modded_name
    FROM slice
  """)

  tp.query("""
    DROP VIEW IF EXISTS thread_slice_stack
  """)

  tp.query(f"""
    CREATE VIEW thread_slice_stack AS
    SELECT
      efs.ts,
      efs.dur,
      IFNULL(n.stack_id, -1) AS stack_id,
      t.utid,
      IIF(efs.source_id IS NULL, '[No slice]', IFNULL(
        (
          SELECT GROUP_CONCAT(modded_name, ' > ')
          FROM (
            SELECT p.modded_name
            FROM ancestor_slice(efs.source_id) a
            JOIN modded_names p ON a.id = p.id
            ORDER BY p.depth
          )
        ) || ' > ' || n.modded_name,
        n.modded_name
      )) AS stack_name
    FROM experimental_flat_slice({start_ts}, {end_ts}) efs
    LEFT JOIN modded_names n ON efs.source_id = n.id
    JOIN thread_track t ON t.id = efs.track_id
  """)

  tp.query("""
    DROP TABLE IF EXISTS thread_slice_stack_with_state
  """)

  tp.query("""
    CREATE VIRTUAL TABLE thread_slice_stack_with_state
    USING SPAN_JOIN(
      thread_slice_stack PARTITIONED utid,
      thread_state PARTITIONED utid
    )
  """)

  if process_name:
    where_process = f"AND process.name GLOB '{process_name}'"
  else:
    where_process = ''

  if main_thread:
    where_main_thread = "AND thread.is_main_thread"
  else:
    where_main_thread = ""

  breakdown = tp.query(f"""
    SELECT
      process.name AS process_name,
      thread.name AS thread_name,
      CASE
        WHEN slice.state = 'D' and slice.io_wait
          THEN 'Uninterruptible sleep (IO)'
        WHEN slice.state = 'DK' and slice.io_wait
          THEN 'Uninterruptible sleep + Wake-kill (IO)'
        WHEN slice.state = 'D' and not slice.io_wait
          THEN 'Uninterruptible sleep (non-IO)'
        WHEN slice.state = 'DK' and not slice.io_wait
          THEN 'Uninterruptible sleep + Wake-kill (non-IO)'
        WHEN slice.state = 'D'
          THEN 'Uninterruptible sleep'
        WHEN slice.state = 'DK'
          THEN 'Uninterruptible sleep + Wake-kill'
        WHEN slice.state = 'S' THEN 'Interruptible sleep'
        WHEN slice.state = 'R' THEN 'Runnable'
        WHEN slice.state = 'R+' THEN 'Runnable (Preempted)'
        ELSE slice.state
      END AS state,
      slice.stack_name,
      SUM(slice.dur)/1e6 AS dur_sum,
      MIN(slice.dur/1e6) AS dur_min,
      MAX(slice.dur/1e6) AS dur_max,
      AVG(slice.dur/1e6) AS dur_mean,
      PERCENTILE(slice.dur/1e6, 50) AS dur_median,
      PERCENTILE(slice.dur/1e6, 25) AS dur_25_percentile,
      PERCENTILE(slice.dur/1e6, 75) AS dur_75_percentile,
      PERCENTILE(slice.dur/1e6, 95) AS dur_95_percentile,
      PERCENTILE(slice.dur/1e6, 99) AS dur_99_percentile,
      COUNT(1) as count
    FROM process
    JOIN thread USING (upid)
    JOIN thread_slice_stack_with_state slice USING (utid)
    WHERE dur != -1
      {where_main_thread}
      {where_process}
    GROUP BY thread.name, stack_id, state
    ORDER BY dur_sum DESC
  """).as_pandas_dataframe()

  return breakdown


def compute_breakdown_for_startup(tp: TraceProcessor,
                                  package_name=None,
                                  process_name=None,
                                  main_thread: bool = False):
  """Computes the slice breakdown (like |compute_breakdown|) but only
  considering slices which happened during an app startup

  Args:
    tp: the trace processor instance to query.
    package_name: optional package name to filter for startups. Only a single
        startup matching this package name should be present. If not specified,
        only a single startup of any app should be in the trace.
    process_name: optional process name to filter for slices; specifying
        this argument can make computing the breakdown a lot faster.
    main_thread: whether to only consider slices on the main thread of each
        process.

  Returns:
    The same as |compute_breakdown| but only containing slices which happened
    during app startup.
  """

  # Verify there was only one startup in the trace matching the package
  # name.
  filter = "WHERE package = '{}'".format(package_name) if package_name else ''
  launches = tp.query(f'''
    INCLUDE PERFETTO MODULE android.startup.startups;

    SELECT ts, ts_end, dur
    FROM android_startups
    {filter}
  ''').as_pandas_dataframe()
  if len(launches) == 0:
    raise TraceProcessorException("Didn't find startup in trace")
  if len(launches) > 1:
    raise TraceProcessorException("Found multiple startups in trace")

  start = launches['ts'][0]
  end = launches['ts_end'][0]

  return compute_breakdown(tp, start, end, process_name, main_thread)
