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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {SqlTable} from '../dev.perfetto.SqlModules/sql_modules';

// Custom table definition for slices using the viz module
export const SLICE_TABLE: SqlTable = {
  name: '_viz_slices_for_ui_table',
  description: 'Slices',
  type: 'table',
  columns: [
    {name: 'id', type: {kind: 'id', source: {table: 'slice', column: 'id'}}},
    {name: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    {name: 'dur', type: PerfettoSqlTypes.DURATION},
    {name: 'category', type: PerfettoSqlTypes.STRING},
    {name: 'name', type: PerfettoSqlTypes.STRING},
    {
      name: 'utid',
      type: {kind: 'joinid', source: {table: 'thread', column: 'id'}},
    },
    {
      name: 'upid',
      type: {kind: 'joinid', source: {table: 'process', column: 'id'}},
    },
    {
      name: 'track_id',
      type: {kind: 'joinid', source: {table: 'track', column: 'id'}},
    },
    {name: 'arg_set_id', type: PerfettoSqlTypes.ARG_SET_ID},
    {name: 'depth', type: PerfettoSqlTypes.INT},
    {
      name: 'parent_id',
      type: {kind: 'joinid', source: {table: 'slice', column: 'id'}},
    },
  ],
};

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.OpenTableCommands';
  static readonly dependencies = [SqlModulesPlugin];
  static readonly description =
    'Adds commands to open some common opinionated tables in table explorer';

  async onTraceLoad(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.slice',
      name: 'Open table: slice',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        sqlModules?.openTableExplorer(SLICE_TABLE.name, {
          customTables: [SLICE_TABLE],
          preamble: 'INCLUDE PERFETTO MODULE viz.slices;',
          initialColumns: [
            {field: 'id'},
            {field: 'ts'},
            {field: 'dur'},
            {field: 'category'},
            {field: 'name'},
            {field: 'utid'},
            {field: 'utid.tid'},
            {field: 'utid.name'},
            {field: 'upid'},
            {field: 'upid.pid'},
            {field: 'upid.name'},
            {field: 'track_id'},
            {field: 'track_id.name'},
            {field: 'arg_set_id'},
          ],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread',
      name: 'Open table: thread',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        sqlModules?.openTableExplorer('thread', {
          initialColumns: [
            {field: 'utid'},
            {field: 'tid'},
            {field: 'name'},
            {field: 'start_ts'},
            {field: 'end_ts'},
            {field: 'upid'},
            {field: 'upid.pid'},
            {field: 'upid.name'},
            {field: 'is_main_thread'},
          ],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.process',
      name: 'Open table: process',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        sqlModules?.openTableExplorer('process', {
          initialColumns: [
            {field: 'upid'},
            {field: 'pid'},
            {field: 'name'},
            {field: 'start_ts'},
            {field: 'end_ts'},
            {field: 'parent_upid'},
            {field: 'uid'},
            {field: 'android_appid'},
            {field: 'machine_id'},
            {field: 'arg_set_id'},
          ],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.sched',
      name: 'Open table: sched',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        sqlModules?.openTableExplorer('sched', {
          initialColumns: [
            {field: 'id'},
            {field: 'ts'},
            {field: 'dur'},
            {field: 'cpu'},
            {field: 'priority'},
            {field: 'utid'},
            {field: 'utid.tid'},
            {field: 'utid.name'},
            {field: 'utid.upid.pid'},
            {field: 'utid.upid.name'},
            {field: 'end_state'},
            {field: 'ucpu'},
          ],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread_state',
      name: 'Open table: thread_state',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        sqlModules?.openTableExplorer('thread_state', {
          initialColumns: [
            {field: 'id'},
            {field: 'ts'},
            {field: 'dur'},
            {field: 'state'},
            {field: 'cpu'},
            {field: 'utid'},
            {field: 'utid.tid'},
            {field: 'utid.name'},
            {field: 'utid.upid.pid'},
            {field: 'utid.upid.name'},
            {field: 'io_wait'},
            {field: 'blocked_function'},
            {field: 'waker_utid'},
            {field: 'waker_utid.tid'},
            {field: 'waker_utid.name'},
            {field: 'waker_id'},
            {field: 'irq_context'},
            {field: 'ucpu'},
          ],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.android_logs',
      name: 'Open table: android_logs',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        sqlModules?.openTableExplorer('android_logs', {
          initialColumns: [
            {field: 'id'},
            {field: 'ts'},
            {field: 'tag'},
            {field: 'prio'},
            {field: 'utid'},
            {field: 'msg'},
          ],
        });
      },
    });
  }
}
