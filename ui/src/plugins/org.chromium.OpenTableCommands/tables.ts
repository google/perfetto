// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {SqlTableDescription} from '../../components/widgets/sql/table/table_description';
import {
  ArgSetIdColumn,
  DurationColumn,
  ProcessIdColumn,
  SchedIdColumn,
  SliceIdColumn,
  StandardColumn,
  ThreadIdColumn,
  ThreadStateIdColumn,
  TimestampColumn,
} from '../../components/widgets/sql/table/columns';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import {Trace} from '../../public/trace';

export function getThreadTable(trace: Trace): SqlTableDescription {
  return {
    name: 'thread',
    columns: [
      new ThreadIdColumn(trace, 'utid', {type: 'id'}),
      new StandardColumn('tid', PerfettoSqlTypes.INT),
      new StandardColumn('name', PerfettoSqlTypes.STRING),
      new TimestampColumn(trace, 'start_ts'),
      new TimestampColumn(trace, 'end_ts'),
      new ProcessIdColumn(trace, 'upid', {notNull: true}),
      new StandardColumn('is_main_thread', PerfettoSqlTypes.BOOLEAN),
    ],
  };
}

export function getProcessTable(trace: Trace): SqlTableDescription {
  return {
    name: 'process',
    columns: [
      new ProcessIdColumn(trace, 'upid', {type: 'id'}),
      new StandardColumn('pid', PerfettoSqlTypes.INT),
      new StandardColumn('name', PerfettoSqlTypes.STRING),
      new TimestampColumn(trace, 'start_ts'),
      new TimestampColumn(trace, 'end_ts'),
      new ProcessIdColumn(trace, 'parent_upid'),
      new StandardColumn('uid', PerfettoSqlTypes.INT),
      new StandardColumn('android_appid', PerfettoSqlTypes.INT),
      new StandardColumn('cmdline', PerfettoSqlTypes.STRING, {
        startsHidden: true,
      }),
      new StandardColumn('machine_id', PerfettoSqlTypes.INT),
      new ArgSetIdColumn('arg_set_id'),
    ],
  };
}

export function getSliceTable(trace: Trace): SqlTableDescription {
  return {
    imports: ['viz.slices'],
    name: '_viz_slices_for_ui_table',
    displayName: 'Slices',
    columns: [
      new SliceIdColumn(trace, 'id', {notNull: true, type: 'id'}),
      new TimestampColumn(trace, 'ts'),
      new DurationColumn(trace, 'dur'),
      new StandardColumn('category', PerfettoSqlTypes.STRING),
      new StandardColumn('name', PerfettoSqlTypes.STRING),
      new StandardColumn('track_id', PerfettoSqlTypes.INT, {
        startsHidden: true,
      }),
      new ThreadIdColumn(trace, 'utid'),
      new ProcessIdColumn(trace, 'upid'),
      new StandardColumn('depth', PerfettoSqlTypes.INT, {startsHidden: true}),
      new SliceIdColumn(trace, 'parent_id'),
      new ArgSetIdColumn('arg_set_id'),
    ],
  };
}

export function getAndroidLogsTable(trace: Trace): SqlTableDescription {
  return {
    name: 'android_logs',
    columns: [
      new StandardColumn('id', PerfettoSqlTypes.INT),
      new TimestampColumn(trace, 'ts'),
      new StandardColumn('tag', PerfettoSqlTypes.STRING),
      new StandardColumn('prio', PerfettoSqlTypes.INT),
      new ThreadIdColumn(trace, 'utid'),
      new ProcessIdColumn(trace, {
        column: 'upid',
        source: {
          table: 'thread',
          joinOn: {utid: 'utid'},
        },
      }),
      new StandardColumn('msg', PerfettoSqlTypes.STRING),
    ],
  };
}

export function getSchedTable(trace: Trace): SqlTableDescription {
  return {
    name: 'sched',
    columns: [
      new SchedIdColumn(trace, 'id'),
      new TimestampColumn(trace, 'ts'),
      new DurationColumn(trace, 'dur'),
      new StandardColumn('cpu', PerfettoSqlTypes.INT),
      new StandardColumn('priority', PerfettoSqlTypes.INT),
      new ThreadIdColumn(trace, 'utid'),
      new ProcessIdColumn(trace, {
        column: 'upid',
        source: {
          table: 'thread',
          joinOn: {utid: 'utid'},
        },
      }),
      new StandardColumn('end_state', PerfettoSqlTypes.STRING),
      new StandardColumn('ucpu', PerfettoSqlTypes.INT, {startsHidden: true}),
    ],
  };
}

export function getThreadStateTable(trace: Trace): SqlTableDescription {
  return {
    name: 'thread_state',
    columns: [
      new ThreadStateIdColumn(trace, 'id'),
      new TimestampColumn(trace, 'ts'),
      new DurationColumn(trace, 'dur'),
      new StandardColumn('state', PerfettoSqlTypes.STRING),
      new StandardColumn('cpu', PerfettoSqlTypes.INT),
      new ThreadIdColumn(trace, 'utid'),
      new ProcessIdColumn(trace, {
        column: 'upid',
        source: {
          table: 'thread',
          joinOn: {utid: 'utid'},
        },
      }),
      new StandardColumn('io_wait', PerfettoSqlTypes.BOOLEAN),
      new StandardColumn('blocked_function', PerfettoSqlTypes.STRING),
      new ThreadIdColumn(trace, 'waker_utid'),
      new ThreadStateIdColumn(trace, 'waker_id'),
      new StandardColumn('irq_context', PerfettoSqlTypes.INT),
      new StandardColumn('ucpu', PerfettoSqlTypes.INT, {startsHidden: true}),
    ],
  };
}
