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
            'id',
            'ts',
            'dur',
            'category',
            'name',
            'utid',
            'utid.tid',
            'utid.name',
            'upid',
            'upid.pid',
            'upid.name',
            'track_id',
            'track_id.name',
            'arg_set_id',
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
            'utid',
            'tid',
            'name',
            'start_ts',
            'end_ts',
            'upid',
            'upid.pid',
            'upid.name',
            'is_main_thread',
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
            'upid',
            'pid',
            'name',
            'start_ts',
            'end_ts',
            'parent_upid',
            'uid',
            'android_appid',
            'machine_id',
            'arg_set_id',
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
            'id',
            'ts',
            'dur',
            'cpu',
            'priority',
            'utid',
            'utid.tid',
            'utid.name',
            'utid.upid.pid',
            'utid.upid.name',
            'end_state',
            'ucpu',
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
            'id',
            'ts',
            'dur',
            'state',
            'cpu',
            'utid',
            'utid.tid',
            'utid.name',
            'utid.upid.pid',
            'utid.upid.name',
            'io_wait',
            'blocked_function',
            'waker_utid',
            'waker_utid.tid',
            'waker_utid.name',
            'waker_id',
            'irq_context',
            'ucpu',
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
          initialColumns: ['id', 'ts', 'tag', 'prio', 'utid', 'msg'],
        });
      },
    });
  }
}
