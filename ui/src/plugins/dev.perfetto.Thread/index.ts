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

import {sqlTableRegistry} from '../../frontend/widgets/sql/table/sql_table_registry';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {getThreadTable} from './table';
import {extensions} from '../../public/lib/extensions';

class ThreadPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace) {
    sqlTableRegistry['thread'] = getThreadTable();
    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.thread',
      name: 'Open table: thread',
      callback: () => {
        extensions.addSqlTableTab(ctx, {
          table: getThreadTable(),
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.Thread',
  plugin: ThreadPlugin,
};
