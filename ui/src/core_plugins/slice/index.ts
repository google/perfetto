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

import {addSqlTableTab} from '../../frontend/sql_table_tab_interface';
import {sqlTableRegistry} from '../../frontend/widgets/sql/table/sql_table_registry';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {getSliceTable} from './table';
import {AsyncAndThreadSliceSelectionAggregator} from './async_and_thread_slice_selection_aggregator';

class SlicePlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace) {
    ctx.selection.registerAreaSelectionAggreagtor(
      new AsyncAndThreadSliceSelectionAggregator(),
    );

    sqlTableRegistry['slice'] = getSliceTable();
    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.slice',
      name: 'Open table: slice',
      callback: () => {
        addSqlTableTab(ctx, {
          table: getSliceTable(),
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Slice',
  plugin: SlicePlugin,
};
