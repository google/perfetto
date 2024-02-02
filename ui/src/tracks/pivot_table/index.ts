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

import {PIVOT_TABLE_REDUX_FLAG} from '../../controller/pivot_table_controller';
import {globals} from '../../frontend/globals';
import {PivotTable} from '../../frontend/pivot_table';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

class PivotTablePlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    if (PIVOT_TABLE_REDUX_FLAG.get()) {
      ctx.registerTab({
        isEphemeral: false,
        uri: 'perfetto.PivotTable#PivotTable',
        content: {
          render: () => m(PivotTable, {
            selectionArea:
                globals.state.nonSerializableState.pivotTable.selectionArea,
          }),
          getTitle: () => 'Pivot Table',
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.PivotTable',
  plugin: PivotTablePlugin,
};
