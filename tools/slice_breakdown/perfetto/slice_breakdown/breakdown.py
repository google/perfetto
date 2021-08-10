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


def compute_breakdown(tp, start_ts=None, end_ts=None, process_name=None):
  bounds = tp.query('SELECT * FROM trace_bounds').as_pandas_dataframe()
  start_ts = start_ts if start_ts else bounds['start_ts'][0]
  end_ts = end_ts if end_ts else bounds['end_ts'][0]

  tp.query("""
    DROP VIEW IF EXISTS modded_names
  """)

  tp.query("""
    CREATE VIEW modded_names AS
    SELECT
      slice.id,
      slice.depth,
      slice.stack_id,
      CASE
        WHEN slice.name LIKE 'Choreographer#doFrame%'
          THEN 'Choreographer#doFrame'
        WHEN slice.name LIKE 'DrawFrames%'
          THEN 'DrawFrames'
        WHEN slice.name LIKE '/data/app%.apk'
          THEN 'APK load'
        WHEN slice.name LIKE 'OpenDexFilesFromOat%'
          THEN 'OpenDexFilesFromOat'
        WHEN slice.name LIKE 'Open oat file%'
          THEN 'Open oat file'
        ELSE slice.name
      END AS modded_name
    FROM slice
  """)

  tp.query("""
    DROP VIEW IF EXISTS thread_slice_stack
  """)

  tp.query("""
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
    FROM experimental_flat_slice({}, {}) efs
    LEFT JOIN modded_names n ON efs.source_id = n.id
    JOIN thread_track t ON t.id = efs.track_id
  """.format(start_ts, end_ts))

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
    select_process = ''
    where_process = "AND process.name = '{}'".format(process_name)
  else:
    select_process = 'process.name AS process_name,'
    where_process = ''

  breakdown = tp.query("""
    SELECT
      {}
      thread.name AS thread_name,
      slice.stack_name,
      slice.state,
      SUM(slice.dur)/1e6 AS dur_sum
    FROM process
    JOIN thread USING (upid)
    JOIN thread_slice_stack_with_state slice USING (utid)
    WHERE dur != -1 {}
    GROUP BY thread.name, stack_id, state
    ORDER BY dur_sum DESC
  """.format(select_process, where_process)).as_pandas_dataframe()

  return breakdown
