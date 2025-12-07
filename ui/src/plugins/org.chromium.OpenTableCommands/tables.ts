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
import {createTableColumn} from '../../components/widgets/sql/table/columns';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import {Trace} from '../../public/trace';

export function getThreadTable(trace: Trace): SqlTableDescription {
  return {
    name: 'thread',
    columns: [
      createTableColumn({
        trace,
        column: 'utid',
        type: {kind: 'id', source: {table: 'thread', column: 'id'}},
      }),
      createTableColumn({trace, column: 'tid', type: PerfettoSqlTypes.INT}),
      createTableColumn({trace, column: 'name', type: PerfettoSqlTypes.STRING}),
      createTableColumn({
        trace,
        column: 'start_ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'end_ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'upid',
        type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'is_main_thread',
        type: PerfettoSqlTypes.BOOLEAN,
      }),
    ],
  };
}

export function getProcessTable(trace: Trace): SqlTableDescription {
  return {
    name: 'process',
    columns: [
      createTableColumn({
        trace,
        column: 'upid',
        type: {kind: 'id', source: {table: 'process', column: 'id'}},
      }),
      createTableColumn({trace, column: 'pid', type: PerfettoSqlTypes.INT}),
      createTableColumn({trace, column: 'name', type: PerfettoSqlTypes.STRING}),
      createTableColumn({
        trace,
        column: 'start_ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'end_ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'parent_upid',
        type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
      }),
      createTableColumn({trace, column: 'uid', type: PerfettoSqlTypes.INT}),
      createTableColumn({
        trace,
        column: 'android_appid',
        type: PerfettoSqlTypes.INT,
      }),
      createTableColumn({
        trace,
        column: 'cmdline',
        type: PerfettoSqlTypes.STRING,
        startsHidden: true,
      }),
      createTableColumn({
        trace,
        column: 'machine_id',
        type: PerfettoSqlTypes.INT,
      }),
      createTableColumn({
        trace,
        column: 'arg_set_id',
        type: PerfettoSqlTypes.ARG_SET_ID,
      }),
    ],
  };
}

export function getSliceTable(trace: Trace): SqlTableDescription {
  return {
    imports: ['viz.slices'],
    name: '_viz_slices_for_ui_table',
    displayName: 'Slices',
    columns: [
      createTableColumn({
        trace,
        column: 'id',
        type: {kind: 'id', source: {table: 'slice', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'dur',
        type: PerfettoSqlTypes.DURATION,
      }),
      createTableColumn({
        trace,
        column: 'category',
        type: PerfettoSqlTypes.STRING,
      }),
      createTableColumn({trace, column: 'name', type: PerfettoSqlTypes.STRING}),
      createTableColumn({
        trace,
        column: 'utid',
        type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'upid',
        type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'track_id',
        type: {kind: 'joinid', source: {table: 'track', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'arg_set_id',
        type: PerfettoSqlTypes.ARG_SET_ID,
      }),
      createTableColumn({
        trace,
        column: 'depth',
        type: PerfettoSqlTypes.INT,
        startsHidden: true,
      }),
      createTableColumn({
        trace,
        column: 'parent_id',
        type: {kind: 'joinid', source: {table: 'slice', column: 'id'}},
        startsHidden: true,
      }),
    ],
  };
}

export function getAndroidLogsTable(trace: Trace): SqlTableDescription {
  return {
    name: 'android_logs',
    columns: [
      createTableColumn({trace, column: 'id', type: PerfettoSqlTypes.INT}),
      createTableColumn({
        trace,
        column: 'ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({trace, column: 'tag', type: PerfettoSqlTypes.STRING}),
      createTableColumn({trace, column: 'prio', type: PerfettoSqlTypes.INT}),
      createTableColumn({
        trace,
        column: 'utid',
        type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
      }),
      createTableColumn({trace, column: 'msg', type: PerfettoSqlTypes.STRING}),
    ],
  };
}

export function getSchedTable(trace: Trace): SqlTableDescription {
  return {
    name: 'sched',
    columns: [
      createTableColumn({
        trace,
        column: 'id',
        type: {kind: 'id', source: {table: 'sched', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'dur',
        type: PerfettoSqlTypes.DURATION,
      }),
      createTableColumn({trace, column: 'cpu', type: PerfettoSqlTypes.INT}),
      createTableColumn({
        trace,
        column: 'priority',
        type: PerfettoSqlTypes.INT,
      }),
      createTableColumn({
        trace,
        column: 'utid',
        type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'end_state',
        type: PerfettoSqlTypes.STRING,
      }),
      createTableColumn({trace, column: 'ucpu', type: PerfettoSqlTypes.INT}),
    ],
  };
}

export function getThreadStateTable(trace: Trace): SqlTableDescription {
  return {
    name: 'thread_state',
    columns: [
      createTableColumn({
        trace,
        column: 'id',
        type: {kind: 'id', source: {table: 'thread_state', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'ts',
        type: PerfettoSqlTypes.TIMESTAMP,
      }),
      createTableColumn({
        trace,
        column: 'dur',
        type: PerfettoSqlTypes.DURATION,
      }),
      createTableColumn({
        trace,
        column: 'state',
        type: PerfettoSqlTypes.STRING,
      }),
      createTableColumn({trace, column: 'cpu', type: PerfettoSqlTypes.INT}),
      createTableColumn({
        trace,
        column: 'utid',
        type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'io_wait',
        type: PerfettoSqlTypes.BOOLEAN,
      }),
      createTableColumn({
        trace,
        column: 'blocked_function',
        type: PerfettoSqlTypes.STRING,
      }),
      createTableColumn({
        trace,
        column: 'waker_utid',
        type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'waker_id',
        type: {kind: 'joinid', source: {table: 'thread_state', column: 'id'}},
      }),
      createTableColumn({
        trace,
        column: 'irq_context',
        type: PerfettoSqlTypes.INT,
      }),
      createTableColumn({trace, column: 'ucpu', type: PerfettoSqlTypes.INT}),
    ],
  };
}
