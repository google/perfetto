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
import {
  THREAD_TABLE,
  PROCESS_TABLE,
  SLICE_TABLE,
  ANDROID_LOGS_TABLE,
  SCHED_TABLE,
  THREAD_STATE_TABLE,
} from '../../components/widgets/sql/table_definitions';
import {extensions} from '../../components/extensions';

export default class implements PerfettoPlugin {
  static readonly id = 'org.Chromium.OpenTableCommands';

  async onTraceLoad(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.slice',
      name: 'Open table: slice',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: SLICE_TABLE,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread',
      name: 'Open table: thread',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: THREAD_TABLE,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.process',
      name: 'Open table: process',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: PROCESS_TABLE,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.sched',
      name: 'Open table: sched',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: SCHED_TABLE,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.thread_state',
      name: 'Open table: thread_state',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: THREAD_STATE_TABLE,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'org.chromium.ShowTable.android_logs',
      name: 'Open table: android_logs',
      callback: () => {
        extensions.addLegacySqlTableTab(ctx, {
          table: ANDROID_LOGS_TABLE,
        });
      },
    });
  }
}
