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

import {sqlTableRegistry} from '../../components/widgets/sql/table/sql_table_registry';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {
  getThreadTable,
  getProcessTable,
  getSliceTable,
  getAndroidLogsTable,
  getSchedTable,
  getThreadStateTable,
} from './tables';
import {extensions} from '../../components/extensions';

export default class implements PerfettoPlugin {
  static readonly id = 'org.Chromium.OpenTableCommands';

  async onTraceLoad(ctx: Trace) {
    sqlTableRegistry['slice'] = getSliceTable;
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.slice',
      name: 'Open table: slice',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: getSliceTable(ctx),
        });
      },
    });

    sqlTableRegistry['thread'] = getThreadTable;
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread',
      name: 'Open table: thread',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: getThreadTable(ctx),
        });
      },
    });

    sqlTableRegistry['process'] = getThreadTable;
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.process',
      name: 'Open table: process',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: getProcessTable(ctx),
        });
      },
    });

    sqlTableRegistry['sched'] = getSchedTable;
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.sched',
      name: 'Open table: sched',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: getSchedTable(ctx),
        });
      },
    });

    sqlTableRegistry['thread_state'] = getThreadStateTable;
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread_state',
      name: 'Open table: thread_state',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: getThreadStateTable(ctx),
        });
      },
    });

    sqlTableRegistry['android_logs'] = getAndroidLogsTable;
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.android_logs',
      name: 'Open table: android_logs',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: getAndroidLogsTable(ctx),
        });
      },
    });
  }
}
