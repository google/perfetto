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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {
  getThreadTable,
  getProcessTable,
  getSliceTable,
  getAndroidLogsTable,
  getSchedTable,
  getThreadStateTable,
} from './table_definitions';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.OpenTableCommands';

  async onTraceLoad(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.slice',
      name: 'Open table: slice',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        const sliceTable = getSliceTable();
        sqlModules?.openTableExplorer(sliceTable.name, {
          customTables: [sliceTable, getThreadTable(), getProcessTable()],
          preamble: 'INCLUDE PERFETTO MODULE viz.slices;',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread',
      name: 'Open table: thread',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        const threadTable = getThreadTable();
        sqlModules?.openTableExplorer(threadTable.name, {
          customTables: [threadTable, getProcessTable()],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.process',
      name: 'Open table: process',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        const processTable = getProcessTable();
        sqlModules?.openTableExplorer(processTable.name, {
          customTables: [processTable],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.sched',
      name: 'Open table: sched',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        const schedTable = getSchedTable();
        sqlModules?.openTableExplorer(schedTable.name, {
          customTables: [schedTable, getThreadTable(), getProcessTable()],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread_state',
      name: 'Open table: thread_state',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        const threadStateTable = getThreadStateTable();
        sqlModules?.openTableExplorer(threadStateTable.name, {
          customTables: [
            threadStateTable,
            getThreadTable(),
            getProcessTable(),
          ],
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.android_logs',
      name: 'Open table: android_logs',
      callback: () => {
        const sqlModules = ctx.plugins.getPlugin(SqlModulesPlugin);
        const androidLogsTable = getAndroidLogsTable();
        sqlModules?.openTableExplorer(androidLogsTable.name, {
          customTables: [androidLogsTable, getThreadTable()],
        });
      },
    });
  }
}
