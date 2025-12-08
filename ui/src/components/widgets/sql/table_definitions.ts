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

import {SqlTableDefinition} from './table/table_description';
import {PerfettoSqlTypes} from '../../../trace_processor/perfetto_sql_type';

// Opinionated definitions of commonly used SQL tables.
// These are raw data definitions without UI dependencies.
// Use resolveTableDefinition() to convert to SqlTableDescription for rendering.

export const THREAD_TABLE: SqlTableDefinition = {
  name: 'thread',
  columns: [
    {
      column: 'utid',
      type: {kind: 'id', source: {table: 'thread', column: 'id'}},
    },
    {column: 'tid', type: PerfettoSqlTypes.INT},
    {column: 'name', type: PerfettoSqlTypes.STRING},
    {column: 'start_ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'end_ts', type: PerfettoSqlTypes.TIMESTAMP},
    {
      column: 'upid',
      type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
    },
    {column: 'is_main_thread', type: PerfettoSqlTypes.BOOLEAN},
  ],
};

export const PROCESS_TABLE: SqlTableDefinition = {
  name: 'process',
  columns: [
    {
      column: 'upid',
      type: {kind: 'id', source: {table: 'process', column: 'id'}},
    },
    {column: 'pid', type: PerfettoSqlTypes.INT},
    {column: 'name', type: PerfettoSqlTypes.STRING},
    {column: 'start_ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'end_ts', type: PerfettoSqlTypes.TIMESTAMP},
    {
      column: 'parent_upid',
      type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
    },
    {column: 'uid', type: PerfettoSqlTypes.INT},
    {column: 'android_appid', type: PerfettoSqlTypes.INT},
    {column: 'cmdline', type: PerfettoSqlTypes.STRING, startsHidden: true},
    {column: 'machine_id', type: PerfettoSqlTypes.INT},
    {column: 'arg_set_id', type: PerfettoSqlTypes.ARG_SET_ID},
  ],
};

export const SLICE_TABLE: SqlTableDefinition = {
  imports: ['viz.slices'],
  name: '_viz_slices_for_ui_table',
  displayName: 'Slices',
  columns: [
    {column: 'id', type: {kind: 'id', source: {table: 'slice', column: 'id'}}},
    {column: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'dur', type: PerfettoSqlTypes.DURATION},
    {column: 'category', type: PerfettoSqlTypes.STRING},
    {column: 'name', type: PerfettoSqlTypes.STRING},
    {
      column: 'utid',
      type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
    },
    {
      column: 'upid',
      type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
    },
    {
      column: 'track_id',
      type: {kind: 'joinid', source: {table: 'track', column: 'id'}},
    },
    {column: 'arg_set_id', type: PerfettoSqlTypes.ARG_SET_ID},
    {column: 'depth', type: PerfettoSqlTypes.INT, startsHidden: true},
    {
      column: 'parent_id',
      type: {kind: 'joinid', source: {table: 'slice', column: 'id'}},
      startsHidden: true,
    },
  ],
};

export const ANDROID_LOGS_TABLE: SqlTableDefinition = {
  name: 'android_logs',
  columns: [
    {column: 'id', type: PerfettoSqlTypes.INT},
    {column: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'tag', type: PerfettoSqlTypes.STRING},
    {column: 'prio', type: PerfettoSqlTypes.INT},
    {
      column: 'utid',
      type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
    },
    {column: 'msg', type: PerfettoSqlTypes.STRING},
  ],
};

export const SCHED_TABLE: SqlTableDefinition = {
  name: 'sched',
  columns: [
    {column: 'id', type: {kind: 'id', source: {table: 'sched', column: 'id'}}},
    {column: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'dur', type: PerfettoSqlTypes.DURATION},
    {column: 'cpu', type: PerfettoSqlTypes.INT},
    {column: 'priority', type: PerfettoSqlTypes.INT},
    {
      column: 'utid',
      type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
    },
    {
      column: {
        column: 'upid',
        source: {
          table: 'thread',
          joinOn: {utid: 'utid'},
        },
      },
      type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
    },
    {column: 'end_state', type: PerfettoSqlTypes.STRING},
    {column: 'ucpu', type: PerfettoSqlTypes.INT},
  ],
};

export const THREAD_STATE_TABLE: SqlTableDefinition = {
  name: 'thread_state',
  columns: [
    {
      column: 'id',
      type: {kind: 'id', source: {table: 'thread_state', column: 'id'}},
    },
    {column: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'dur', type: PerfettoSqlTypes.DURATION},
    {column: 'state', type: PerfettoSqlTypes.STRING},
    {column: 'cpu', type: PerfettoSqlTypes.INT},
    {
      column: 'utid',
      type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
    },
    {
      column: {
        column: 'upid',
        source: {
          table: 'thread',
          joinOn: {utid: 'utid'},
        },
      },
      type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
    },
    {column: 'io_wait', type: PerfettoSqlTypes.BOOLEAN},
    {column: 'blocked_function', type: PerfettoSqlTypes.STRING},
    {
      column: 'waker_utid',
      type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
    },
    {
      column: 'waker_id',
      type: {kind: 'joinid', source: {table: 'thread_state', column: 'id'}},
    },
    {column: 'irq_context', type: PerfettoSqlTypes.INT},
    {column: 'ucpu', type: PerfettoSqlTypes.INT},
  ],
};
